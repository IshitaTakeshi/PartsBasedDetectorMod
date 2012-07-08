/* 
 *  Software License Agreement (BSD License)
 *
 *  Copyright (c) 2012, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage, Inc. nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 *  File:    HOGFeatures.cpp
 *  Author:  Hilton Bristow
 *  Created: Jun 21, 2012
 */

#ifdef _OPENMP
#include <omp.h>
#endif
#include <math.h>
#include <cstdio>
#include <opencv2/imgproc/imgproc.hpp>
#include "HOGFeatures.hpp"
using namespace std;
using namespace cv;

// declare all possible types of specialization (this is kinda sacrilege, but it's all we'll ever need...)
HOGFeatures<float> hog_float;
HOGFeatures<double> hog_double;

/*! @brief Calculate features at multiple scales
 *
 * Features are calculated first at native resolution,
 * then progressively downsampled to coarser spatial
 * resolutions
 *
 * This function supports multithreading via OpenMP
 *
 * @param im the input image at native resolution
 * @return the pyramid of features, fine to coarse, each calculated via
 * features()
 */
template<typename T>
void HOGFeatures<T>::pyramid(const Mat& im, vector<Mat>& pyrafeatures) {

	vector<Mat> pyraimages;
	pyraimages.resize(nscales_);
	pyrafeatures.clear();
	pyrafeatures.resize(nscales_);
	pyraimages.resize(nscales_);
	scales_.clear();
	scales_.resize(nscales_);

	// calculate the scaling factor
	Size_<float> imsize = im.size();
	int interval = ceil((float)nscales_ / 3.0);
	float sc = pow(2.0, 1.0/(float)interval);

	// perform the non-power of two scaling
	#ifdef _OPENMP
	#pragma omp parallel for
	#endif
	for (int i = 0; i < interval; ++i) {
		Mat scaled;
		resize(im, scaled, imsize * (1.0f/pow(sc,i)));
		pyraimages[i] = scaled;
		// perform subsequent power of two scaling
		for (int j = i+interval; j < nscales_; j+=interval) {
			Mat scaled2;
			pyrDown(scaled, scaled2);
			pyraimages[j] = scaled2;
		}
	}

	// perform the actual feature computation, in parallel if possible
	#ifdef _OPENMP
	#pragma omp parallel for
	#endif
	for (int n = 0; n < nscales_; ++n) {
		Mat feature;
		Mat padded;
		switch (im.depth()) {
			case CV_32F: features<float>(pyraimages[n], feature); break;
			case CV_64F: features<double>(pyraimages[n], feature); break;
			case CV_8U:  features<uint8_t>(pyraimages[n], feature); break;
			case CV_16U: features<uint16_t>(pyraimages[n], feature); break;
			default: CV_Error(CV_StsUnsupportedFormat, "Unsupported image type"); break;
		}
		copyMakeBorder(feature, padded, 1, 1, flen_, flen_, BORDER_CONSTANT, 1);
		pyrafeatures[n] = padded;
	}
}

/*! @brief compute the HOG features for an image
 *
 * This method computes the HOG features for an image, given the
 * binsize_ and flen_ class members. The output is effectively a
 * 3D matrix (i,j,k) that has been flattened to a 2D (i,j*k) matrix
 * for faster processing. The (i,j) dimensions represent the resultant
 * spatial size of the response (ie im.size() / binsize_) and the
 * (k) dimension represents the histogram weights (length flen_)
 *
 * The function supports multithreading via OpenMP
 *
 * @param im the input image (must be color of type CV_8UC3)
 * @return the HOG features as a 2D matrix
 */
template<typename T> template<typename IT>
void HOGFeatures<T>::features(const Mat& im, Mat& featm) const {

	// compute the size of the output matrix
	bool color  = (im.channels() == 3);
	Size imsize = im.size();
	Size blocks = imsize;
	blocks.height = round((float)blocks.height / (float)binsize_);
	blocks.width  = round((float)blocks.width  / (float)binsize_);
	Size outsize;
	outsize.width  = max(blocks.width-2,  0);
	outsize.height = max(blocks.height-2, 0);
	Size visible   = blocks*binsize_;

	Mat histm = Mat::zeros(Size(blocks.width*norient_, blocks.height),  DataType<T>::type);
	Mat normm = Mat::zeros(Size(blocks.width,          blocks.height),  DataType<T>::type);
	featm     = Mat::zeros(Size(outsize.width*flen_,   outsize.height), DataType<T>::type);

	// epsilon to avoid division by zero
	const double eps = 0.0001;

	// unit vectors to compute gradient orientation
	const T uu[9] = {1.000, 0.9397, 0.7660, 0.5000, 0.1736, -0.1736, -0.5000, -0.7660, -0.9397};
	const T vv[9] = {0.000, 0.3420, 0.6428, 0.8660, 0.9848,  0.9848,  0.8660,  0.6428,  0.3420};

	// calculate the zero offset
	const IT* src = im.ptr<IT>(0);
	T* hist = histm.ptr<T>(0);
	T* norm = normm.ptr<T>(0);
	T* feat = featm.ptr<T>(0);

	for (int y = 1; y < visible.height-1; ++y) {
		for (int x = 1; x < visible.width-1; ++x) {

			// first image channel
			const IT* s = src + min(x, im.cols-2) + min(y, im.rows-2)*imsize.width;
			T dy = *(s+imsize.width) - *(s-imsize.width);
			T dx = *(s+1) - *(s-1);
			T  v = dx*dx + dy*dy;

			if (color) {
				// second image channel
				s += imsize.width*imsize.height;
				T dyg = *(s+imsize.width) - *(s-imsize.width);
				T dxg = *(s+1) - *(s-1);
				T  vg = dxg*dxg + dyg*dyg;

				// third image channel
				s += imsize.width*imsize.height;
				T dyr = *(s+imsize.width) - *(s-imsize.width);
				T dxr = *(s+1) - *(s-1);
				T  vr = dxr*dxr + dyr*dyr;

				// pick the channel with the strongest gradient
				if (vg > v) { v = vg; dx = dxg; dy = dyg; }
				if (vr > v) { v = vr; dx = dxr; dy = dyr; }
			}

			// snap to one of 18 orientations
			T best_dot = 0;
			int best_o = 0;
			for (int o = 0; o < norient_/2; ++o) {
				T dot = uu[o]*dx + vv[o]*dy;
				if (dot > best_dot) { best_dot = dot; best_o = o; }
				else if (-dot > best_dot) { best_dot = -dot; best_o = o+9; }
			}

			// add to 4 histograms around pixel using linear interpolation
			T yp = ((T)y+0.5)/(T)binsize_ - 0.5;
			T xp = ((T)x+0.5)/(T)binsize_ - 0.5;
			int iyp = (int)floor(yp);
			int ixp = (int)floor(xp);
			T vy0 = yp-iyp;
			T vx0 = xp-ixp;
			T vy1 = 1.0-vy0;
			T vx1 = 1.0-vx0;
			v = sqrt(v);

			if (iyp >= 0 && ixp >= 0) 							*(hist + (iyp*blocks.width + ixp)*norient_ + best_o) = vy1*vx1*v;
			if (iyp >= 0 && ixp+1 < blocks.width) 				*(hist + (iyp*blocks.width + ixp+1)*norient_ + best_o) = vx1*vy0*v;
			if (iyp+1 < blocks.height && ixp >= 0) 				*(hist + ((iyp+1)*blocks.width + ixp)*norient_ + best_o) = vy1*vx0*v;
			if (iyp+1 < blocks.height && ixp+1 < blocks.width)	*(hist + ((iyp+1)*blocks.width + ixp+1)*norient_ + best_o) = vy0*vx0*v;
		}
	}

	// compute the energy in each block by summing over orientations
	for (int i = 0; i < blocks.height*blocks.width; ++i) {
		for (int o = 0; o < 9; ++o) norm[i] += pow(hist[i*norient_+o] + hist[i*norient_+norient_/2+o], 2);
	}

	// compute the features
	for (int y = 0; y < outsize.height; ++y) {
		for (int x = 0; x < outsize.width; ++x) {
			T* dst = feat + (y*outsize.width + x)*flen_;
			T* src, *p, n1, n2, n3, n4;

			p  = norm + (y+1)*outsize.width + (x+1);
			n1 = 1.0f / sqrt(*p + *(p+1) + *(p+outsize.width) + *(p+outsize.width+1) + eps);
			p  = norm + y*outsize.width + (x+1);
			n2 = 1.0f / sqrt(*p + *(p+1) + *(p+outsize.width) + *(p+outsize.width+1) + eps);
			p  = norm + (y+1)*outsize.width + x;
			n3 = 1.0f / sqrt(*p + *(p+1) + *(p+outsize.width) + *(p+outsize.width+1) + eps);
			p  = norm + y*outsize.width + x;
			n4 = 1.0f / sqrt(*p + *(p+1) + *(p+outsize.width) + *(p+outsize.width+1) + eps);

			T t1 = 0, t2 = 0, t3 = 0, t4 = 0;

			// contrast-sensitive features
			src = hist + ((y+1)*blocks.width + (x+1))*norient_;
			for (int o = 0; o < norient_; ++o) {
				T h1 = min(*src * n1, (T)0.2);
				T h2 = min(*src * n2, (T)0.2);
				T h3 = min(*src * n3, (T)0.2);
				T h4 = min(*src * n4, (T)0.2);
				*(dst++) = 0.5 * (h1 + h2 + h3 + h4);
				src++;
				t1 += h1;
				t2 += h2;
				t3 += h3;
				t4 += h4;
			}

			// contrast-insensitive features
			src = hist + ((y+1)*blocks.width + (x+1))*norient_;
			for (int o = 0; o < norient_/2; ++o) {
				T sum = *src + *(src+norient_/2);
				T h1 = min(sum * n1, (T)0.2);
				T h2 = min(sum * n2, (T)0.2);
				T h3 = min(sum * n3, (T)0.2);
				T h4 = min(sum * n4, (T)0.2);
				*(dst++) = 0.5 * (h1 + h2 + h3 + h4);
				src++;
			}

			//texture features
			*(dst++) = 0.2357 * t1;
			*(dst++) = 0.2357 * t2;
			*(dst++) = 0.2357 * t3;
			*(dst++) = 0.2357 * t4;

			// truncation feature
			*dst = 0;
		}
	}
}

/*! @brief Convolve two matrices, with a stride of greater than one
 *
 * This is a specialized 2D convolution algorithm with a stride of greater
 * than one. It is designed to convolve a filter with a feature, where at
 * each pixel an SVM must be evaluated (leading to a stride of SVM weight length).
 * The convolution can be thought of as flattened a 2.5D convolution where the
 * (i,j) dimension is the spatial plane and the (k) dimension is the SVM weights
 * of the pixels. As one would expect, this method is slow
 *
 * The function supports multithreading via OpenMP
 *
 * @param feature the feature matrix
 * @param filter the filter (SVM)
 * @param stride the SVM weight length
 * @return the response (pdf)
 */
template<typename T>
void HOGFeatures<T>::convolve(const Mat& feature, const Mat& filter, Mat& pdf, int stride) {

	// error checking
	assert(feature.depth() == filter.depth());
	assert(feature.cols % stride == 0 && filter.cols % stride == 0);

	// really basic convolution algorithm with a stride greater than one
	const int M = feature.rows - filter.rows + 1;
	const int N = feature.cols - filter.cols + stride;
	const int H = filter.rows;
	const int W = filter.cols;
	pdf.create(Size(M, N), feature.type());
	const T* feat_ptr = feature.ptr<T>(0);
	const T* filt_ptr = filter.ptr<T>(0);
	const T* filt_start = filter.ptr<T>(0);
	T* pdf_ptr = pdf.ptr<T>(0);

	for (int m = 0; m < M; ++m) {
		for (int n = 0; n < N; n+=stride) {
			T accum = 0;
			filt_ptr = filt_start;
			feat_ptr = feature.ptr<T>(m) + n;
			for (int h = 0; h < H; ++h) {
				while (filt_ptr < filt_start+h*W) {
					accum += *(filt_ptr++) * *(feat_ptr++);
				}
				feat_ptr = feature.ptr<T>(m+h) + n;
			}
			*(pdf_ptr++) = accum;
		}
	}
}

/*! @brief Calculate the responses of a set of features to a set of filter experts
 *
 * A response represents the likelihood of the part appearing at each location of
 * the feature map. Parts are support vector machines (SVMs) represented as filters.
 * The convolution of a filter with a feature produces a probability density function
 * (pdf) of part location
 * @param features the input features (at different scales, and by extension, size)
 * @param filters the filters representing the parts across mixtures
 * @return a vector of responses (pdfs)
 */
template<typename T>
void HOGFeatures<T>::pdf(const vector<Mat>& features, const vector<Mat>& filters, vector<Mat>& responses) {

	// preallocate the output
	int M = features.size();
	int N = filters.size();
	responses.clear();
	responses.resize(M*N);
	// iterate
#ifdef _OPENMP
	omp_set_num_threads(omp_get_num_procs());
	#pragma omp parallel for
#endif
	for (int i = 0; i < M*N; ++i) {
		int n = i%N;
		int m = floor(i/N);
		Mat response;
		convolve(features[m], filters[n], response, flen_);
		responses[i] = response;
	}
}