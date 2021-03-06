// This software includes the work that is distributed in the Apache License 2.0

#include "stdafx.h"
#include <dlib/opencv.h>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/opencv.hpp>
#include <dlib/image_processing/frontal_face_detector.h>
#include <dlib/image_processing/render_face_detections.h>
#include <dlib/image_processing.h>
#include <dlib/gui_widgets.h>
#include <librealsense2/rs.hpp> // Include RealSense Cross Platform API

using namespace dlib;
using namespace std;
using namespace cv;

rs2_stream find_stream_to_align(const std::vector<rs2::stream_profile>& streams);
void remove_background(rs2::video_frame& other, const rs2::depth_frame& depth_frame, float depth_scale, float clipping_dist);

int main() {
	// Realsense用のパイプライン作成
	rs2::pipeline pipe;
	rs2::pipeline_profile profile;
	
	try {
		// 設定ファイル作成&適応
		rs2::config cfg; 
		cfg.enable_stream(RS2_STREAM_COLOR, 640, 480, RS2_FORMAT_BGR8, 30); // RGB
		cfg.enable_stream(RS2_STREAM_DEPTH, 640, 480, RS2_FORMAT_Z16, 30); // Depth
		profile = pipe.start(cfg);
	}
	catch (exception& e) {
		cout << e.what() << endl;
		system("PAUSE");
		exit(-1);
	}

	// 適応した設定を再読込、フレーム間の座標関係等を取得
	auto sensor = profile.get_device().first<rs2::depth_sensor>();
	auto depth_scale = sensor.get_depth_scale();
	rs2_stream align_to = find_stream_to_align(profile.get_streams());
	rs2::align align(align_to);

	// RSウォームアップ - 自動露光が安定するまで最初の数フレームは無視
	rs2::frameset frames;
	for (int i = 0; i < 30; i++)
	{
		//Wait for all configured streams to produce a frame
		frames = pipe.wait_for_frames();
	}
	// Declare depth colorizer for pretty visualization of depth data
	rs2::colorizer color_map;

	// 顔認識用モデルのロード
	frontal_face_detector detector = get_frontal_face_detector();
	shape_predictor pose_model;
	try{
		deserialize("shape_predictor_68_face_landmarks.dat") >> pose_model;
	}
	catch (serialization_error& e) {
		cout << "You need dlib's default face landmarking model file to run this example." << endl;
		cout << "You can get it from the following URL: " << endl;
		cout << "   http://dlib.net/files/shape_predictor_68_face_landmarks.dat.bz2" << endl;
		cout << endl << e.what() << endl;
		system("PAUSE");
		exit(-1);
	}

	// dlibで使うWindowシステム
	image_window orig; // 元画像表示用ウインドウ
	image_window win; // 処理結果表示用ウインドウ

	// メインウインドウが閉じられるまでループ
	while (!win.is_closed()) {
		// フレーム取得
		rs2::frameset frameset = pipe.wait_for_frames(); // Wait for next set of frames from the camera
		//rs2::frame depth_frame = color_map(frameset.get_depth_frame()); // Find and colorize the depth data
		//rs2::frame color_frame = frameset.get_color_frame();            // Find the color data

		// カラー画像と深度画像を対応付ける
		auto processed = align.process(frameset);
		rs2::video_frame color_frame = processed.first(align_to);
		rs2::depth_frame depth_frame = processed.get_depth_frame();

		// フレームからMatを生成
		Mat color(Size(640, 480), CV_8UC3, (void*)color_frame.get_data(), Mat::AUTO_STEP);
		Mat depth(Size(640, 480), CV_8UC3, (void*)depth_frame.get_data(), Mat::AUTO_STEP);

		// MATをdlibで扱える形式に変更
		// NOTE: 浅いコピーなので元のcolor,depthをいじってはいけない
		cv_image<bgr_pixel> cimg(color);
		cv_image<bgr_pixel> dimg(depth);

		// 顔認識
		std::vector<dlib::rectangle> faces = detector(cimg);
		std::vector<full_object_detection> shapes;
		for (unsigned long i = 0; i < faces.size(); ++i)
			shapes.push_back(pose_model(cimg, faces[i]));

		full_object_detection face;
		point p;
		Mat ov = Mat::zeros(480, 640, CV_8UC3);

		float depth_clipping_distance = 10.0f; // 背景を除去する距離(m)
		bool faceDetected = false;
		if(shapes.size() > 0) {
			// 検出された顔の数が1つ以上なら
			face = shapes[0]; // 最初の顔の
			p = face.part(27); // 鼻の根元の場所を取得
			faceDetected = true;
			// その位置にマークを追加
			line(ov, Point(p.x() -10, p.y()), Point(p.x() + 10, p.y()), Scalar(100, 200, 200), 1, CV_AA);
			line(ov, Point(p.x(), p.y() -10), Point(p.x(), p.y() + 10), Scalar(100, 200, 200), 1, CV_AA);
			depth_clipping_distance = depth_frame.get_distance(p.x(), p.y());
		}
		cv_image<bgr_pixel> overlay(ov);

		// 元のカラー画像表示
		orig.set_image(cimg);

		// 顔までの距離 + 30cm より後ろの背景を塗りつぶす
		remove_background(color_frame, depth_frame, depth_scale, depth_clipping_distance + 0.30f);
		Mat clipped_color(Size(640, 480), CV_8UC3, (void*)color_frame.get_data(), Mat::AUTO_STEP);
		cv_image<bgr_pixel> clipimg(clipped_color);

		// 結果表示
		win.clear_overlay();
		//win.set_image(cimg); // もとのカラー画像
		win.set_image(clipimg); // 背景除去済み画像
		//win.set_image(overlay); // 顔検出されたマーカーを表示
		win.add_overlay(render_face_detections(shapes));

		// cout<< p.x() << "," << p.y() <<endl; // 顔の鼻の根本座標
		if (faceDetected) cout << "face detected :" << depth_clipping_distance << "m" << endl; // 顔の鼻の根本までの距離表示
		else cout << "no face " << endl;

		if (cv::waitKey(1) == 27) exit(0); // ESCキーで終了
	}
}

	
// 与えられたストリームの中から深度情報を持つプロファイルを見つけ、それと対応可能なストリームを見つける
// librealsense/examples/align/rs-align.cpp
rs2_stream find_stream_to_align(const std::vector<rs2::stream_profile>& streams)
{
	//Given a vector of streams, we try to find a depth stream and another stream to align depth with.
	//We prioritize color streams to make the view look better.
	//If color is not available, we take another stream that (other than depth)
	rs2_stream align_to = RS2_STREAM_ANY;
	bool depth_stream_found = false;
	bool color_stream_found = false;
	for (rs2::stream_profile sp : streams)
	{
		rs2_stream profile_stream = sp.stream_type();
		if (profile_stream != RS2_STREAM_DEPTH)
		{
			if (!color_stream_found)         //Prefer color
				align_to = profile_stream;

			if (profile_stream == RS2_STREAM_COLOR)
			{
				color_stream_found = true;
			}
		}
		else
		{
			depth_stream_found = true;
		}
	}

	if (!depth_stream_found)
		throw std::runtime_error("No Depth stream available");

	if (align_to == RS2_STREAM_ANY)
		throw std::runtime_error("No stream found to align with Depth");

	return align_to;
}

// 深度と対応付られたカラー画像、および深度単位と最大距離を指定すると、その距離以降の背景が灰色に塗りつぶされる
// librealsense/examples/align/rs-align.cpp
void remove_background(rs2::video_frame& other_frame, const rs2::depth_frame& depth_frame, float depth_scale, float clipping_dist)
{
	const uint16_t* p_depth_frame = reinterpret_cast<const uint16_t*>(depth_frame.get_data());
	uint8_t* p_other_frame = reinterpret_cast<uint8_t*>(const_cast<void*>(other_frame.get_data()));

	int width = other_frame.get_width();
	int height = other_frame.get_height();
	int other_bpp = other_frame.get_bytes_per_pixel();

#pragma omp parallel for schedule(dynamic) //Using OpenMP to try to parallelise the loop
	for (int y = 0; y < height; y++)
	{
		auto depth_pixel_index = y * width;
		for (int x = 0; x < width; x++, ++depth_pixel_index)
		{
			// Get the depth value of the current pixel
			auto pixels_distance = depth_scale * p_depth_frame[depth_pixel_index];

			// Check if the depth value is invalid (<=0) or greater than the threashold
			if (pixels_distance <= 0.f || pixels_distance > clipping_dist)
			{
				// Calculate the offset in other frame's buffer to current pixel
				auto offset = depth_pixel_index * other_bpp;

				// Set pixel to "background" color (0x999999)
				std::memset(&p_other_frame[offset], 0x99, other_bpp);
			}
		}
	}
}