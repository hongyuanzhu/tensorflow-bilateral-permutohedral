/*!
 *  \brief     A helper class for {@link MultiStageMeanfieldLayer} class, which is the Caffe layer that implements the
 *             CRF-RNN described in the paper: Conditional Random Fields as Recurrent Neural Networks. IEEE ICCV 2015.
 *
 *             This class itself is not a proper Caffe layer although it behaves like one to some degree.
 *
 *  \authors   Sadeep Jayasumana, Bernardino Romera-Paredes, Shuai Zheng, Zhizhong Su.
 *  \version   1.0
 *  \date      2015
 *  \copyright Torr Vision Group, University of Oxford.
 *  \details   If you use this code, please consider citing the paper:
 *             Shuai Zheng, Sadeep Jayasumana, Bernardino Romera-Paredes, Vibhav Vineet, Zhizhong Su, Dalong Du,
 *             Chang Huang, Philip H. S. Torr. Conditional Random Fields as Recurrent Neural Networks. IEEE ICCV 2015.
 *
 *             For more information about CRF-RNN, please visit the project website http://crfasrnn.torr.vision.
 */
#include <vector>

#include "bilateral_interface.hpp"
#include "util/math_functions.hpp"
using namespace caffe;

namespace tensorflow {

/**
 * To be invoked once only immediately after construction.
 */
template <typename Dtype>
void BilateralInterface<Dtype>::OneTimeSetUp(
    Blob<Dtype>* const input,
    Blob<Dtype>* const featswrt,
    Blob<Dtype>* const wspatial,
    Blob<Dtype>* const wbilateral,
    Blob<Dtype>* const output,
    float stdv_spatial_space,
    float stdv_bilateral_space) {

  // save input/output pointers for when using Forward_x()
  input_ = input;
  featswrt_ = featswrt;
  wspatial_ = wspatial;
  wbilateral_ = wbilateral;
  output_ = output;

  // filter standard deviations
  theta_alpha_ = stdv_bilateral_space;
  theta_gamma_ = stdv_spatial_space;

  // save shapes
  count_ = input->count();
  num_ = input->num();
  channels_ = input->channels();
  height_ = input->height();
  width_ = input->width();
  num_pixels_ = height_ * width_;
  wrt_chans_ = 2 + featswrt->channels();

  // check shapes
  CHECK(num_ == output->num() && height_ == output->height() && width_ == output->width())
      << "input and output must have same number in minibatch and same spatial dimensions!";
  CHECK(num_ == featswrt->num() && height_ == featswrt->height() && width_ == featswrt->width())
      << "input and featswrt must have same number in minibatch and same spatial dimensions!";
  CHECK(channels_ == wspatial->shape(2) && channels_ == wspatial->shape(3))
      << "input and wspatial must have same num channels! "
      <<channels_<<" != "<<wspatial->shape(2)<<" or "<<wspatial->shape(3);
  CHECK(channels_ == wbilateral->shape(2) && channels_ == wbilateral->shape(3))
      << "input and wbilateral must have same num channels! "
      <<channels_<<" != "<<wbilateral->shape(2)<<" or "<<wbilateral->shape(3);

  // intermediate blobs
  spatial_out_blob_.Reshape(num_, channels_, height_, width_);
  bilateral_out_blob_.Reshape(num_, channels_, height_, width_);

  // Initialize the spatial lattice. This does not need to be computed for every image because we use a fixed size.
  float spatial_kernel[2 * num_pixels_];
  float *spatial_kernel_gpu_;
  compute_spatial_kernel(spatial_kernel);
  spatial_lattice_.reset(new ModifiedPermutohedral());
  freebilateralbuffer();

  spatial_norm_.Reshape(1, 1, height_, width_);
  Dtype* norm_data_gpu ;
  Dtype*  norm_data;
  // Initialize the spatial lattice. This does not need to be computed for every image because we use a fixed size.
  switch (Caffe::mode()) {
    case Caffe::CPU:
      norm_data = spatial_norm_.mutable_cpu_data();
      spatial_lattice_->init(spatial_kernel, 2, width_, height_);
      // Calculate spatial filter normalization factors.
      norm_feed_= new Dtype[num_pixels_];
      caffe_set(num_pixels_, Dtype(1.0), norm_feed_);
      // pass norm_feed and norm_data to gpu
      spatial_lattice_->compute(norm_data, norm_feed_, 1);
      bilateral_kernel_buffer_ = new float[wrt_chans_ * num_pixels_];
      init_cpu = true;
      break;
    #ifndef CPU_ONLY
    case Caffe::GPU:
      CUDA_CHECK(cudaMalloc((void**)&spatial_kernel_gpu_, 2*num_pixels_ * sizeof(float))) ;
      CUDA_CHECK(cudaMemcpy(spatial_kernel_gpu_, spatial_kernel, 2*num_pixels_ * sizeof(float), cudaMemcpyHostToDevice)) ;
      spatial_lattice_->init(spatial_kernel_gpu_, 2, width_, height_);
      CUDA_CHECK(cudaMalloc((void**)&norm_feed_, num_pixels_ * sizeof(Dtype))) ;
      caffe_gpu_set(num_pixels_, Dtype(1.0), norm_feed_);
      norm_data_gpu = spatial_norm_.mutable_gpu_data();
      spatial_lattice_->compute(norm_data_gpu, norm_feed_, 1);
      norm_data = spatial_norm_.mutable_cpu_data();
      CUDA_CHECK(cudaMalloc((void**)&bilateral_kernel_buffer_, wrt_chans_ * num_pixels_ * sizeof(float))) ;
      CUDA_CHECK(cudaFree(spatial_kernel_gpu_));
      init_gpu = true;
      break;
    #endif
    default:
    LOG(FATAL) << "Unknown caffe mode.";
  }

  for (int i = 0; i < num_pixels_; ++i) {
    norm_data[i] = 1.0f / (norm_data[i] + 1e-20f);
  }

  // Allocate space for bilateral kernels. This is a temporary buffer used to compute bilateral lattices later.
  // Also allocate space for holding bilateral filter normalization values.
  bilateral_norms_.Reshape(num_, 1, height_, width_);
}

/**
 * Forward pass during the inference.
 */
template <typename Dtype>
void BilateralInterface<Dtype>::Forward_cpu() {

  // Initialize the bilateral lattices.
  bilateral_lattices_.resize(num_);
  for (int n = 0; n < num_; ++n) {

    compute_bilateral_kernel(featswrt_, n, bilateral_kernel_buffer_);
    bilateral_lattices_[n].reset(new ModifiedPermutohedral());
    bilateral_lattices_[n]->init(bilateral_kernel_buffer_, wrt_chans_, width_, height_);

    // Calculate bilateral filter normalization factors.
    Dtype* norm_output_data = bilateral_norms_.mutable_cpu_data() + bilateral_norms_.offset(n);
    bilateral_lattices_[n]->compute(norm_output_data, norm_feed_, 1);
    for (int i = 0; i < num_pixels_; ++i) {
      norm_output_data[i] = 1.f / (norm_output_data[i] + 1e-20f);
    }
  }

  //------------------------------- Softmax normalization--------------------
  //softmax_layer_->Forward(softmax_bottom_vec_, softmax_top_vec_);

  //-----------------------------------Message passing-----------------------
  for (int n = 0; n < num_; ++n) {

    Dtype* spatial_out_data = spatial_out_blob_.mutable_cpu_data() + spatial_out_blob_.offset(n);
    const Dtype* prob_input_data = input_->cpu_data() + input_->offset(n);

    spatial_lattice_->compute(spatial_out_data, prob_input_data, channels_, false);

    // Pixel-wise normalization.
    for (int channel_id = 0; channel_id < channels_; ++channel_id) {
      caffe_mul(num_pixels_, spatial_norm_.cpu_data(),
          spatial_out_data + channel_id * num_pixels_,
          spatial_out_data + channel_id * num_pixels_);
    }

    Dtype* bilateral_out_data = bilateral_out_blob_.mutable_cpu_data() + bilateral_out_blob_.offset(n);

    bilateral_lattices_[n]->compute(bilateral_out_data, prob_input_data, channels_, false);
    // Pixel-wise normalization.
    for (int channel_id = 0; channel_id < channels_; ++channel_id) {
      caffe_mul(num_pixels_, bilateral_norms_.cpu_data() + bilateral_norms_.offset(n),
          bilateral_out_data + channel_id * num_pixels_,
          bilateral_out_data + channel_id * num_pixels_);
    }
  }

  caffe_set(count_, Dtype(0.), output_->mutable_cpu_data());

  for (int n = 0; n < num_; ++n) {
    caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, channels_, num_pixels_, channels_, (Dtype) 1.,
        wspatial_->cpu_data(), spatial_out_blob_.cpu_data() + spatial_out_blob_.offset(n), (Dtype) 0.,
        output_->mutable_cpu_data() + output_->offset(n));
  }

  for (int n = 0; n < num_; ++n) {
    caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, channels_, num_pixels_, channels_, (Dtype) 1.,
        wbilateral_->cpu_data(), bilateral_out_blob_.cpu_data() + bilateral_out_blob_.offset(n), (Dtype) 1.,
        output_->mutable_cpu_data() + output_->offset(n));
  }

  //--------------------------- Compatibility multiplication ----------------
  //Result from message passing needs to be multiplied with compatibility values.
  /*for (int n = 0; n < num_; ++n) {
    caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasNoTrans, channels_, num_pixels_,
        channels_, (Dtype) 1., this->blobs_[2]->cpu_data(),
        output_->cpu_data() + output_->offset(n), (Dtype) 0.,
        pairwise_.mutable_cpu_data() + pairwise_.offset(n));
  }*/
  //------------------------- Adding unaries, normalization is left to the next iteration --------------
  // Add unary
  //sum_layer_->Forward(sum_bottom_vec_, sum_top_vec_);
}


template<typename Dtype>
void BilateralInterface<Dtype>::Backward_cpu() {
  std::cout<<"TODO: BilateralInterface<Dtype>::Backward_cpu()"<<std::endl;
  assert(False);
#if 0
  //---------------------------- Add unary gradient --------------------------
  //vector<bool> eltwise_propagate_down(2, true);
  //sum_layer_->Backward(sum_top_vec_, eltwise_propagate_down, sum_bottom_vec_);
  //---------------------------- Update compatibility diffs ------------------
  /*caffe_set(this->blobs_[2]->count(), Dtype(0.), this->blobs_[2]->mutable_cpu_diff());

  for (int n = 0; n < num_; ++n) {
    caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasTrans, channels_, channels_,
                          num_pixels_, (Dtype) 1., pairwise_.cpu_diff() + pairwise_.offset(n),
                          output_->cpu_data() + output_->offset(n), (Dtype) 1.,
                          this->blobs_[2]->mutable_cpu_diff());
  }
  //-------------------------- Gradient after compatibility transform--- -----
  for (int n = 0; n < num_; ++n) {
    caffe_cpu_gemm<Dtype>(CblasTrans, CblasNoTrans, channels_, num_pixels_,
                          channels_, (Dtype) 1., this->blobs_[2]->cpu_data(),
                          pairwise_.cpu_diff() + pairwise_.offset(n), (Dtype) 0.,
                          output_->mutable_cpu_diff() + output_->offset(n));
  }*/

  // ------------------------- Gradient w.r.t. kernels weights ------------
  caffe_set(wspatial_->count(), Dtype(0.), wspatial_->mutable_cpu_diff());
  caffe_set(wbilateral_->count(), Dtype(0.), wbilateral_->mutable_cpu_diff());

  for (int n = 0; n < num_; ++n) {
    caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasTrans, channels_, channels_,
                          num_pixels_, (Dtype) 1., output_->cpu_diff() + output_->offset(n),
                          spatial_out_blob_.cpu_data() + spatial_out_blob_.offset(n), (Dtype) 1.,
                          wspatial_->mutable_cpu_diff());
  }

  for (int n = 0; n < num_; ++n) {
    caffe_cpu_gemm<Dtype>(CblasNoTrans, CblasTrans, channels_, channels_,
                          num_pixels_, (Dtype) 1., output_->cpu_diff() + output_->offset(n),
                          bilateral_out_blob_.cpu_data() + bilateral_out_blob_.offset(n), (Dtype) 1.,
                          wbilateral_->mutable_cpu_diff());
  }

  /*Dtype* tmp = new Dtype[count_];
  caffe_mul<Dtype>(count_, output_->cpu_diff(), spatial_out_blob_.cpu_data(), tmp);
  for (int c = 0; c < count_; ++c) {
    (wspatial_->mutable_cpu_diff())[0] += tmp[c];
  }
  caffe_mul<Dtype>(count_, output_->cpu_diff(), bilateral_out_blob_.cpu_data(), tmp);
  for (int c = 0; c < count_; ++c) {
    (wbilateral_->mutable_cpu_diff())[0] += tmp[c];
  }
  delete[] tmp;*/

  // TODO: Check whether there's a way to improve the accuracy of this calculation.
  for (int n = 0; n < num_; ++n) {
    caffe_cpu_gemm<Dtype>(CblasTrans, CblasNoTrans, channels_, num_pixels_, channels_, (Dtype) 1.,
                          wspatial_->cpu_data(), output_->cpu_diff() + output_->offset(n),
                          (Dtype) 0.,
                          spatial_out_blob_.mutable_cpu_diff() + spatial_out_blob_.offset(n));
  }
  //caffe_cpu_scale<Dtype>(count_, (wspatial_->cpu_data())[0],
  //    output_->cpu_diff(), spatial_out_blob_.mutable_cpu_diff());

  for (int n = 0; n < num_; ++n) {
    caffe_cpu_gemm<Dtype>(CblasTrans, CblasNoTrans, channels_, num_pixels_, channels_, (Dtype) 1.,
                          wbilateral_->cpu_data(), output_->cpu_diff() + output_->offset(n),
                          (Dtype) 0.,
                          bilateral_out_blob_.mutable_cpu_diff() + bilateral_out_blob_.offset(n));
  }
  //caffe_cpu_scale<Dtype>(count_, (wbilateral_->cpu_data())[0],
  //    output_->cpu_diff(), bilateral_out_blob_.mutable_cpu_diff());


  //---------------------------- BP thru normalization --------------------------
  for (int n = 0; n < num_; ++n) {

    Dtype *spatial_out_diff = spatial_out_blob_.mutable_cpu_diff() + spatial_out_blob_.offset(n);
    for (int channel_id = 0; channel_id < channels_; ++channel_id) {
      caffe_mul(num_pixels_, spatial_norm_.cpu_data(),
                spatial_out_diff + channel_id * num_pixels_,
                spatial_out_diff + channel_id * num_pixels_);
    }

    Dtype *bilateral_out_diff = bilateral_out_blob_.mutable_cpu_diff() + bilateral_out_blob_.offset(n);
    for (int channel_id = 0; channel_id < channels_; ++channel_id) {
      caffe_mul(num_pixels_, bilateral_norms_.cpu_data() + bilateral_norms_.offset(n),
                bilateral_out_diff + channel_id * num_pixels_,
                bilateral_out_diff + channel_id * num_pixels_);
    }
  }

  //--------------------------- Gradient for message passing ---------------
  for (int n = 0; n < num_; ++n) {

    spatial_lattice_->compute(input_->mutable_cpu_diff() + input_->offset(n),
                              spatial_out_blob_.cpu_diff() + spatial_out_blob_.offset(n), channels_,
                              true, false);

    bilateral_lattices_[n]->compute(input_->mutable_cpu_diff() + input_->offset(n),
                                       bilateral_out_blob_.cpu_diff() + bilateral_out_blob_.offset(n),
                                       channels_, true, true);
  }

  //--------------------------------------------------------------------------------
  //vector<bool> propagate_down(2, true);
  //softmax_layer_->Backward(softmax_top_vec_, propagate_down, softmax_bottom_vec_);
#endif
}


template<typename Dtype>
void BilateralInterface<Dtype>::freebilateralbuffer() {
  if(bilateral_kernel_buffer_ != NULL) {
    if(init_cpu){
        delete[] bilateral_kernel_buffer_;
        bilateral_kernel_buffer_ = NULL;
    }
  #ifndef CPU_ONLY
    if(init_gpu){
        CUDA_CHECK(cudaFree(bilateral_kernel_buffer_));
        bilateral_kernel_buffer_ = NULL;
    }
  #endif
  }
  if(norm_feed_ != NULL) {
    if(init_cpu){
        delete[] norm_feed_;
        norm_feed_ = NULL;
    }
  #ifndef CPU_ONLY
    if(init_gpu){
        CUDA_CHECK(cudaFree(norm_feed_));
        norm_feed_ = NULL;
    }
  #endif
  }
}

template<typename Dtype>
void BilateralInterface<Dtype>::compute_bilateral_kernel(const Blob<Dtype>* const rgb_blob, const int n,
                                                               float* const output_kernel) {

  for (int p = 0; p < num_pixels_; ++p) {
    output_kernel[wrt_chans_ * p] = static_cast<float>(p % width_) / theta_alpha_;
    output_kernel[wrt_chans_ * p + 1] = static_cast<float>(p / width_) / theta_alpha_;

    const Dtype * const rgb_data_start = rgb_blob->cpu_data() + rgb_blob->offset(n);
    for(int cc=2; cc<wrt_chans_; ++cc) {
      output_kernel[wrt_chans_ * p + cc] = static_cast<float>((rgb_data_start + num_pixels_*(cc-2))[p]);
    }
  }
}

template <typename Dtype>
void BilateralInterface<Dtype>::compute_spatial_kernel(float* const output_kernel) {

  for (int p = 0; p < num_pixels_; ++p) {
    output_kernel[2*p] = static_cast<float>(p % width_) / theta_gamma_;
    output_kernel[2*p + 1] = static_cast<float>(p / width_) / theta_gamma_;
  }
}


/*	Compile certain expected uses of BilateralInterface.
	Will cause compiliation errors ("undefined reference") if you use another type not defined here.
*/
template class BilateralInterface<float>;
template class BilateralInterface<double>;


}  // namespace caffe
