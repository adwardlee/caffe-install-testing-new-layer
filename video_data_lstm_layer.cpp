#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "opencv2/core/core.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"

#include "caffe/layer.hpp"
#include "caffe/proto/caffe.pb.h"
#include "caffe/util/io.hpp"
#include "caffe/util/math_functions.hpp"
#include "caffe/util/rng.hpp"
#include "caffe/data_transformer.hpp"
#include "caffe/internal_thread.hpp"
#include "caffe/layers/base_data_layer.hpp"
#include "caffe/layers/video_data_layer.hpp"
#include "caffe/util/benchmark.hpp"


#ifdef USE_MPI
#include "mpi.h"
#include <boost/filesystem.hpp>
using namespace boost::filesystem;
#endif

namespace caffe{
template <typename Dtype>
VideoDataLayer<Dtype>:: ~VideoDataLayer<Dtype>(){
	this->StopInternalThread();
}

template <typename Dtype>
void VideoDataLayer<Dtype>:: DataLayerSetUp(const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top){
	const int new_height  = this->layer_param_.video_data_param().new_height();
	const int new_width  = this->layer_param_.video_data_param().new_width();
	const int new_length  = this->layer_param_.video_data_param().new_length();
	const int num_segments = this->layer_param_.video_data_param().num_segments();
	const string& source = this->layer_param_.video_data_param().source();

	LOG(INFO) << "Opening file: " << source;
	std:: ifstream infile(source.c_str());
	string filename;
	int label;
	int length;
	while (infile >> filename >> length >> label){
		lines_.push_back(std::make_pair(filename,label));
		lines_duration_.push_back(length);
	}
	if (this->layer_param_.video_data_param().shuffle()){
		const unsigned int prefectch_rng_seed = caffe_rng_rand();
		prefetch_rng_1_.reset(new Caffe::RNG(prefectch_rng_seed));
		prefetch_rng_2_.reset(new Caffe::RNG(prefectch_rng_seed));
		ShuffleVideos();
	}

	LOG(INFO) << "A total of " << lines_.size() << " videos.";
	lines_id_ = 0;

	//check name patter
	if (this->layer_param_.video_data_param().name_pattern() == ""){
		if (this->layer_param_.video_data_param().modality() == VideoDataParameter_Modality_RGB){
			name_pattern_ = "image_%04d.jpg";
		}else if (this->layer_param_.video_data_param().modality() == VideoDataParameter_Modality_FLOW){
			name_pattern_ = "flow_%c_%04d.jpg";
		}
	}else{
		name_pattern_ = this->layer_param_.video_data_param().name_pattern();
	}

	Datum datum;
	const unsigned int frame_prefectch_rng_seed = caffe_rng_rand();
	frame_prefetch_rng_.reset(new Caffe::RNG(frame_prefectch_rng_seed));
	int average_duration = (int) lines_duration_[lines_id_]/num_segments;
	vector<int> offsets;
	for (int i = 0; i < num_segments; ++i){
		caffe::rng_t* frame_rng = static_cast<caffe::rng_t*>(frame_prefetch_rng_->generator());
		int offset = (*frame_rng)() % (average_duration - new_length + 1);
		offsets.push_back(offset+i*average_duration);
	}
	if (this->layer_param_.video_data_param().modality() == VideoDataParameter_Modality_FLOW)
		CHECK(ReadSegmentFlowToDatum(lines_[lines_id_].first, lines_[lines_id_].second,
									 offsets, new_height, new_width, new_length, &datum, name_pattern_.c_str()));
	else
		CHECK(ReadSegmentRGBToDatum(lines_[lines_id_].first, lines_[lines_id_].second,
									offsets, new_height, new_width, new_length, &datum, true, name_pattern_.c_str()));
	const int crop_size = this->layer_param_.transform_param().crop_size();
	const int batch_size = this->layer_param_.video_data_param().batch_size();	
	CHECK_GT(batch_size, 0) << "Positive batch size required";////llj
	const int frames = new_length * new_segments;	/////llj


	if (crop_size > 0){

		top[0]->Reshape(frames  * batch_size, datum.channels(), crop_size, crop_size);
	  for (int i = 0; i < this->prefetch_.size(); ++i) { //llj
		this->prefetch_[i]->data_.Reshape(frames * batch_size, datum.channels(), crop_size, crop_size);
	  }
	} else {
	
		top[0]->Reshape(frames  *batch_size, datum.channels(), datum.height(), datum.width());
	  for (int i = 0; i < this->prefetch_.size(); ++i) {	
		this->prefetch_[i]->data_.Reshape(frames  *batch_size, datum.channels(), datum.height(), datum.width());
	  }	
	}
	LOG(INFO) << "output data size: " << top[0]->num() << "," << top[0]->channels() << "," << top[0]->height() << "," << top[0]->width();

	top[1]->Reshape(frames, batch_size, 1, 1);
	for (int i = 0; i < this->prefetch_.size(); ++i) {
		this->prefetch_[i]->label_.Reshape(frames, batch_size, 1, 1);
	}
	top[2]->Reshape(frames, batch_size, 1, 1)
	for (int i = 0; i < this->prefetch_.size(); ++i) {
		this->prefetch_[i]->clip_marker_.Reshape(batch_size, 1, 1, 1);
	}
	vector<int> top_shape = this->data_transformer_->InferBlobShape(datum);
	this->transformed_data_.Reshape(top_shape);
}

template <typename Dtype>
void VideoDataLayer<Dtype>::ShuffleVideos(){
	caffe::rng_t* prefetch_rng1 = static_cast<caffe::rng_t*>(prefetch_rng_1_->generator());
	caffe::rng_t* prefetch_rng2 = static_cast<caffe::rng_t*>(prefetch_rng_2_->generator());
	shuffle(lines_.begin(), lines_.end(), prefetch_rng1);
	shuffle(lines_duration_.begin(), lines_duration_.end(),prefetch_rng2);
}

template <typename Dtype>
void VideoDataLayer<Dtype>::load_batch(Batch<Dtype>* batch){

	Datum datum;
	CHECK(batch->data_.count());
	Dtype* top_data = batch->data_.mutable_cpu_data();
	Dtype* top_label = batch->label_.mutable_cpu_data();
	VideoDataParameter video_data_param = this->layer_param_.video_data_param();
	const int batch_size = video_data_param.batch_size();
	const int new_height = video_data_param.new_height();
	const int new_width = video_data_param.new_width();
	const int new_length = video_data_param.new_length();
	const int num_segments = video_data_param.num_segments();
	const int lines_size = lines_.size();

	for (int item_id = 0; item_id < batch_size; ++item_id){
		CHECK_GT(lines_size, lines_id_);
		vector<int> offsets;
		int average_duration = (int) lines_duration_[lines_id_] / num_segments;
		for (int i = 0; i < num_segments; ++i){
			if (this->phase_==TRAIN){
				if (average_duration >= new_length){
					caffe::rng_t* frame_rng = static_cast<caffe::rng_t*>(frame_prefetch_rng_->generator());
					int offset = (*frame_rng)() % (average_duration - new_length + 1);
					offsets.push_back(offset+i*average_duration);
				} else {
					offsets.push_back(0);
				}
			} else{
				if (average_duration >= new_length)
				offsets.push_back(int((average_duration-new_length+1)/2 + i*average_duration));
				else
				offsets.push_back(0);
			}
		}
		if (this->layer_param_.video_data_param().modality() == VideoDataParameter_Modality_FLOW){
			if(!ReadSegmentFlowToDatum(lines_[lines_id_].first, lines_[lines_id_].second,
									   offsets, new_height, new_width, new_length, &datum, name_pattern_.c_str())) {
				continue;
			}
		} else{
			if(!ReadSegmentRGBToDatum(lines_[lines_id_].first, lines_[lines_id_].second,
									  offsets, new_height, new_width, new_length, &datum, true, name_pattern_.c_str())) {
				continue;
			}
		}

		int offset1 = batch->data_.offset(item_id);
    	        this->transformed_data_.set_cpu_data(top_data + offset1);
		this->data_transformer_->Transform(datum, &(this->transformed_data_));
		top_label[item_id] = lines_[lines_id_].second;
		//LOG()

		//next iteration
		lines_id_++;
		if (lines_id_ >= lines_size) {
			DLOG(INFO) << "Restarting data prefetching from start.";
			lines_id_ = 0;
			if(this->layer_param_.video_data_param().shuffle()){
				ShuffleVideos();
			}
		}
	}
}

INSTANTIATE_CLASS(VideoDataLSTMLayer);
REGISTER_LAYER_CLASS(VideoDataLSTM);
}
