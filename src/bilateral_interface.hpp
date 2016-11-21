#ifndef CAFFE_BilateralInterface_LAYER_HPP_
#define CAFFE_BilateralInterface_LAYER_HPP_
#include <string>
#include <utility>
#include <vector>
#include <boost/shared_array.hpp>
#include <boost/shared_ptr.hpp>
using boost::shared_ptr;
using std::vector;

#include "util/blob.hpp"
#include "util/modified_permutohedral.hpp"

namespace tensorflow {


template <typename Dtype>
class BilateralInterface {

 public:
  /**
   * Must be invoked only once after the construction of the layer.
   */
  void OneTimeSetUp(
      const TensorShape & input,
      const TensorShape & featswrt,
      const TensorShape & wspatial,
      const TensorShape & wbilateral,
      float stdv_spatial_space,
      float stdv_bilateral_space);
  void OneTimeSetUp(
      Blob<Dtype>* const input,
      Blob<Dtype>* const featswrt,
      Blob<Dtype>* const wspatial,
      Blob<Dtype>* const wbilateral,
      float stdv_spatial_space,
      float stdv_bilateral_space);

  /**
   * Forward pass - to be called during inference.
   */
  virtual void Forward_cpu(
                Blob<Dtype>* const input,
                Blob<Dtype>* const featswrt,
                Blob<Dtype>* const wspatial,
                Blob<Dtype>* const wbilateral,
                Blob<Dtype>* const output);
//#ifndef CPU_ONLY
//  virtual void Forward_gpu();
//#endif

  /**
   * Backward pass - to be called during training.
   */
  virtual void Backward_cpu();
//#ifndef CPU_ONLY
//  virtual void Backward_gpu();
//#endif

  void compute_spatial_kernel(float* const output_kernel);
  void compute_bilateral_kernel(const Blob<Dtype>* const rgb_blob, const int n, float* const output_kernel);

  explicit BilateralInterface() : init_cpu(false), init_gpu(false),
                                  norm_feed_(nullptr),
                                  bilateral_kernel_buffer_(nullptr) {}
  ~BilateralInterface() {freebilateralbuffer();}

 protected:
  void OneTimeSetUp_KnownShapes();
  void freebilateralbuffer();

  // filter radii
  float theta_alpha_; // spatial part of bilateral
  //Dtype theta_beta_; // "color" part of bilateral, assumed to be 1.0, otherwise you must scale your data first!
  float theta_gamma_; // spatial of spatial

  int count_;
  int num_;
  int channels_;
  int height_;
  int width_;
  int num_pixels_;
  int wrt_chans_;

  // which device was it initialized on?
  bool init_cpu;
  bool init_gpu;


  Dtype* norm_feed_;

  // outputs of spatial and bilateral filters, respectively
  Blob<Dtype> spatial_out_blob_;
  Blob<Dtype> bilateral_out_blob_;

  float* bilateral_kernel_buffer_;
  shared_ptr<caffe::ModifiedPermutohedral> spatial_lattice_;
  vector<shared_ptr<caffe::ModifiedPermutohedral> > bilateral_lattices_;

  Blob<Dtype> spatial_norm_;
  Blob<Dtype> bilateral_norms_;
};

} //namespace

#endif