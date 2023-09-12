/*
 * Copyright (c) 2023, NVIDIA CORPORATION.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include "../common/ann_types.hpp"

#define FMT_HEADER_ONLY
#include <raft/core/logger.hpp>

#include <faiss/IndexFlat.h>
#include <faiss/IndexIVFFlat.h>
#include <faiss/IndexIVFPQ.h>
#include <faiss/IndexRefine.h>
#include <faiss/IndexScalarQuantizer.h>
#include <faiss/index_io.h>
#include <omp.h>

#include <cassert>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {

faiss::MetricType parse_metric_type(raft::bench::ann::Metric metric)
{
  if (metric == raft::bench::ann::Metric::kInnerProduct) {
    return faiss::METRIC_INNER_PRODUCT;
  } else if (metric == raft::bench::ann::Metric::kEuclidean) {
    return faiss::METRIC_L2;
  } else {
    throw std::runtime_error("faiss supports only metric type of inner product and L2");
  }
}

// note BLAS library can still use multi-threading, and
// setting environment variable like OPENBLAS_NUM_THREADS can control it
class OmpSingleThreadScope {
 public:
  OmpSingleThreadScope()
  {
    max_threads_ = omp_get_max_threads();
    omp_set_num_threads(1);
  }
  ~OmpSingleThreadScope()
  {
    // the best we can do
    omp_set_num_threads(max_threads_);
  }

 private:
  int max_threads_;
};

}  // namespace

namespace raft::bench::ann {

template <typename T>
class Faiss : public ANN<T> {
 public:
  using typename ANN<T>::AnnSearchParam;
  struct SearchParam : public AnnSearchParam {
    int nprobe;
    float refine_ratio = 1.0;
  };

  struct BuildParam {
    int nlist = 1;
    int ratio = 2;
  };

  Faiss(Metric metric, int dim, const BuildParam& param)
    : ANN<T>(metric, dim),
      metric_type_(parse_metric_type(metric)),
      nlist_{param.nlist},
      training_sample_fraction_{1.0 / double(param.ratio)}
  {
    static_assert(std::is_same_v<T, float>, "faiss support only float type");
  }

  virtual ~Faiss() noexcept {}

  void build(const T* dataset, size_t nrow, cudaStream_t stream = 0) final;

  void set_search_param(const AnnSearchParam& param) override;

  void init_quantizer(int dim)
  {
    if (this->metric_type_ == faiss::MetricType::METRIC_L2) {
      this->quantizer_ = std::make_unique<faiss::IndexFlatL2>(dim);
    } else if (this->metric_type_ == faiss::MetricType::METRIC_INNER_PRODUCT) {
      this->quantizer_ = std::make_unique<faiss::IndexFlatIP>(dim);
    }
  }

  // TODO: if the number of results is less than k, the remaining elements of 'neighbors'
  // will be filled with (size_t)-1
  void search(const T* queries,
              int batch_size,
              int k,
              size_t* neighbors,
              float* distances,
              cudaStream_t stream = 0) const final;

  AlgoProperty get_preference() const override
  {
    AlgoProperty property;
    // to enable building big dataset which is larger than  memory
    property.dataset_memory_type = MemoryType::Host;
    property.query_memory_type   = MemoryType::Host;
    return property;
  }

 protected:
  template <typename Index>
  void save_(const std::string& file) const;

  template <typename Index>
  void load_(const std::string& file);

  std::unique_ptr<faiss::Index> index_;
  std::unique_ptr<faiss::Index> quantizer_;
  std::unique_ptr<faiss::IndexRefineFlat> index_refine_;
  faiss::MetricType metric_type_;
  int nlist_;
  double training_sample_fraction_;
};

template <typename T>
void Faiss<T>::build(const T* dataset, size_t nrow, cudaStream_t stream)
{
  OmpSingleThreadScope omp_single_thread;
  auto index_ivf = dynamic_cast<faiss::IndexIVF*>(index_.get());
  if (index_ivf != nullptr) {
    // set the min/max training size for clustering to use the whole provided training set.
    double trainset_size       = training_sample_fraction_ * static_cast<double>(nrow);
    double points_per_centroid = trainset_size / static_cast<double>(nlist_);
    int max_ppc                = std::ceil(points_per_centroid);
    int min_ppc                = std::floor(points_per_centroid);
    if (min_ppc < index_ivf->cp.min_points_per_centroid) {
      RAFT_LOG_WARN(
        "The suggested training set size %zu (data size %zu, training sample ratio %f) yields %d "
        "points per cluster (n_lists = %d). This is smaller than the FAISS default "
        "min_points_per_centroid = %d.",
        static_cast<size_t>(trainset_size),
        nrow,
        training_sample_fraction_,
        min_ppc,
        nlist_,
        index_ivf->cp.min_points_per_centroid);
    }
    index_ivf->cp.max_points_per_centroid = max_ppc;
    index_ivf->cp.min_points_per_centroid = min_ppc;
  }
  index_->train(nrow, dataset);  // faiss::IndexFlat::train() will do nothing
  assert(index_->is_trained);
  index_->add(nrow, dataset);
}

template <typename T>
void Faiss<T>::set_search_param(const AnnSearchParam& param)
{
  auto search_param = dynamic_cast<const SearchParam&>(param);
  int nprobe        = search_param.nprobe;
  assert(nprobe <= nlist_);
  dynamic_cast<faiss::IndexIVF*>(index_.get())->nprobe = nprobe;

  if (search_param.refine_ratio > 1.0) {
    this->index_refine_ = std::make_unique<faiss::IndexRefineFlat>(this->index_.get());
    this->index_refine_.get()->k_factor = search_param.refine_ratio;
  }
}

template <typename T>
void Faiss<T>::search(const T* queries,
                      int batch_size,
                      int k,
                      size_t* neighbors,
                      float* distances,
                      cudaStream_t stream) const
{
  static_assert(sizeof(size_t) == sizeof(faiss::Index::idx_t),
                "sizes of size_t and faiss::Index::idx_t are different");
  index_->search(
    batch_size, queries, k, distances, reinterpret_cast<faiss::Index::idx_t*>(neighbors));
}

template <typename T>
template <typename Index>
void Faiss<T>::save_(const std::string& file) const
{
  OmpSingleThreadScope omp_single_thread;
  faiss::write_index(index_.get(), file.c_str());
}

template <typename T>
template <typename Index>
void Faiss<T>::load_(const std::string& file)
{
  OmpSingleThreadScope omp_single_thread;
  index_ = std::unique_ptr<Index>(dynamic_cast<Index*>(faiss::read_index(file.c_str())));
}

template <typename T>
class FaissIVFFlat : public Faiss<T> {
 public:
  using typename Faiss<T>::BuildParam;

  FaissIVFFlat(Metric metric, int dim, const BuildParam& param) : Faiss<T>(metric, dim, param)
  {
    this->init_quantizer(dim);
    this->index_ = std::make_unique<faiss::IndexIVFFlat>(
      this->quantizer_.get(), dim, param.nlist, this->metric_type_);
  }

  void save(const std::string& file) const override
  {
    this->template save_<faiss::IndexIVFFlat>(file);
  }
  void load(const std::string& file) override { this->template load_<faiss::IndexIVFFlat>(file); }
};

template <typename T>
class FaissIVFPQ : public Faiss<T> {
 public:
  struct BuildParam : public Faiss<T>::BuildParam {
    int M;
    int bitsPerCode;
    bool usePrecomputed;
  };

  FaissIVFPQ(Metric metric, int dim, const BuildParam& param) : Faiss<T>(metric, dim, param)
  {
    this->init_quantizer(dim);
    this->index_ = std::make_unique<faiss::IndexIVFPQ>(
      this->quantizer_.get(), dim, param.nlist, param.M, param.bitsPerCode, this->metric_type_);
  }

  void save(const std::string& file) const override
  {
    this->template save_<faiss::IndexIVFPQ>(file);
  }
  void load(const std::string& file) override { this->template load_<faiss::IndexIVFPQ>(file); }
};

template <typename T>
class FaissIVFSQ : public Faiss<T> {
 public:
  struct BuildParam : public Faiss<T>::BuildParam {
    std::string quantizer_type;
  };

  FaissIVFSQ(Metric metric, int dim, const BuildParam& param) : Faiss<T>(metric, dim, param)
  {
    faiss::ScalarQuantizer::QuantizerType qtype;
    if (param.quantizer_type == "fp16") {
      qtype = faiss::ScalarQuantizer::QT_fp16;
    } else if (param.quantizer_type == "int8") {
      qtype = faiss::ScalarQuantizer::QT_8bit;
    } else {
      throw std::runtime_error("FaissIVFSQ supports only fp16 and int8 but got " +
                               param.quantizer_type);
    }

    this->init_quantizer(dim);
    this->index_ = std::make_unique<faiss::IndexIVFScalarQuantizer>(
      this->quantizer_.get(), dim, param.nlist, qtype, this->metric_type_, true);
  }

  void save(const std::string& file) const override
  {
    this->template save_<faiss::IndexIVFScalarQuantizer>(file);
  }
  void load(const std::string& file) override
  {
    this->template load_<faiss::IndexIVFScalarQuantizer>(file);
  }
};

template <typename T>
class FaissFlat : public Faiss<T> {
 public:
  FaissFlat(Metric metric, int dim) : Faiss<T>(metric, dim, typename Faiss<T>::BuildParam{})
  {
    this->index_ = std::make_unique<faiss::IndexFlat>(dim, this->metric_type_);
  }

  // class Faiss is more like a IVF class, so need special treating here
  void set_search_param(const typename ANN<T>::AnnSearchParam&) override{};

  void save(const std::string& file) const override
  {
    this->template save_<faiss::IndexFlat>(file);
  }
  void load(const std::string& file) override { this->template load_<faiss::IndexFlat>(file); }
};

}  // namespace raft::bench::ann
