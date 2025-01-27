// LazyImage.cpp : Defines the exported functions for the DLL application.
//

#include "stdafx.h"
#include "lodepng.h"
#include "image.h"
#include <thread>
#include <mutex>
#include <iostream>
#include <ostream>

//Allows Gamemaker to call functions decorated with this.
#define gmx extern "C" __declspec(dllexport)

using ds_map = int;

const int EVENT_OTHER_SOCIAL = 70;

//thread and image binders to allow reference from GM.

std::vector<std::thread*> threads;
std::vector<int> thread_slots;
std::mutex thread_key;

std::vector<image*> images;
std::vector<int> image_slots;
std::mutex image_key;


//Define GM interop functions.
void(*gml_event_perform_async)(ds_map map, int event_type) = nullptr;
int(*gml_ds_map_create)(int n, ...) = nullptr;
bool(*gml_ds_map_add_double)(ds_map map, const char* key, double value) = nullptr;
bool(*gml_ds_map_add_string)(ds_map map, const char* key, const char* value) = nullptr;

ds_map ds_map_create() {
	return gml_ds_map_create(0);
}

//Detaches a thread without locking. Make sure calling code locks using thread_key before invoking.
int unsafe_thread_detach(int handle) {
	if (threads.size() > handle && threads[handle] != NULL) {
		if (threads[handle]->joinable())
			threads[handle]->detach();
		delete threads[handle];
		threads[handle] = NULL;
		thread_slots.push_back(handle);
		return 1;
	}

	return 0;
}

//Example 1
//Decode from disk to raw pixels with a single function call
void decodeOneStep(const char* filename) {
	std::vector<unsigned char> image; //the raw pixels
	unsigned width, height;

	//decode
	unsigned error = lodepng::decode(image, width, height, filename);

	//if there's an error, display it
	if (error) std::cout << "decoder error " << error << ": " << lodepng_error_text(error) << std::endl;

	//the pixels are now in the vector "image", 4 bytes per pixel, ordered RGBARGBA..., use it as texture, draw it, ...
}

//loads an image from a file asynchronously.
void load_image(const char* image_path, int handle) {
	image* loaded = new image();
	int index;
	unsigned int error = lodepng::decode(loaded->data, loaded->width, loaded->height, image_path);

	if (error == 0) {
		// Swap Red and Green values to match Gamemaker sprite layout.
		auto data = loaded->data;
		for (int i = 0; i < data.size(); i += 4) {
			auto temp = data[i];
			//data[i] = data[i + 2];
			//data[i + 2] = temp;
		}
		loaded->data = data;
		std::lock_guard<std::mutex> image_lock(image_key);
		if (image_slots.empty()) {
			index = images.size();
			images.push_back(loaded);
		}
		else {
			index = image_slots.back();
			image_slots.pop_back();
			images[index] = loaded;
		}
	}
	else {
		index = -1;
	}

	std::lock_guard<std::mutex> lock(thread_key);
	if (threads.size() > handle && threads[handle] != NULL) {
		ds_map map = ds_map_create();

		//Originally added the file path to the map,
		//but the data would be deleted before it could be read from GM.
		//To fix the issue, you'd have to memcopy the string, but then you'd have a mem leak,
		//as the new data would never be freed.
		//Instead, we now return the thread handle which the user can save when initiating the thread,
		//which can be used to determine which image was loaded.
		gml_ds_map_add_string(map, "type", "image_loaded");
		gml_ds_map_add_string(map, "path", image_path);
		gml_ds_map_add_double(map, "handle", handle);
		gml_ds_map_add_double(map, "image", index);
		gml_ds_map_add_double(map, "error", error);
		gml_event_perform_async(map, EVENT_OTHER_SOCIAL);

		//frees itself.
		unsafe_thread_detach(handle);
	}
}

//loads a buffer with image data synchronously.
void load_buffer(char* out, image source) {
	memcpy(out, source.data.data(), source.data.size());
}

//function to be executed on a seperate thread to load a buffer with image data.
void load_buffer_async(char* out, image source, int index, int handle, int image_index) {
	load_buffer(out, source);
	std::lock_guard<std::mutex> lock(thread_key);
	if (threads.size() > handle && threads[handle] != NULL) {
		ds_map map = ds_map_create();
		gml_ds_map_add_string(map, "type", "buffer_loaded");
		gml_ds_map_add_double(map, "buffer", index);
		gml_ds_map_add_double(map, "image", image_index);
		gml_ds_map_add_double(map, "handle", handle);
		gml_event_perform_async(map, EVENT_OTHER_SOCIAL);

		//frees itself.
		unsafe_thread_detach(handle);
	}
}

//Gets size of the data saved by the image in bytes.
gmx double get_image_size(double image_index) {
	unsigned int index = (unsigned int)image_index;
	if (images.size() <= index || images[index] == NULL)
		return -1;
	return images[(unsigned int)image_index]->data.size();
}

//Gets the x dimension of an image.
gmx double get_image_width(double image_index) {
	unsigned int index = (unsigned int)image_index;
	if (images.size() <= index || images[index] == NULL)
		return -1;
	return images[index]->width;
}

//Gets the y dimension of an image.
gmx double get_image_height(double image_index) {
	unsigned int index = (unsigned int)image_index;
	if (images.size() <= index || images[index] == NULL)
		return -1;
	return images[index]->height;
}

//Creates a new thread that will load an image from a file. Returns the thread id.
gmx double load_image_async(const char* file_name) {
	int index;
	std::lock_guard<std::mutex> lock(thread_key);

	if (thread_slots.empty()) {
		index = threads.size();
		// Need to copy the string,
		// when this is not done, the memory appears to be overwritten
		std::string image_path = std::string(file_name);
		threads.push_back(new std::thread(load_image, image_path.c_str(), index));
	}
	else {
		index = thread_slots.back();
		thread_slots.pop_back();
		// Need to copy the string,
		// when this is not done, the memory appears to be overwritten
		std::string image_path = std::string(file_name);
		threads[index] = new std::thread(load_image, image_path.c_str(), index);
	}

	return index;
}

//Creates a new thread that loads image data into a buffer. Returns the thread id.
gmx double load_image_data_async(char* buffer_address, double buffer_id, double image_id) {
	int thread_index;
	int image_index = (int)image_id;
	image source;

	// Create a new scope to control image_lock lifetime.
	{
		std::lock_guard<std::mutex> image_lock(image_key);
		if (images.size() <= image_index || images[image_index] == NULL)
			return -1;
		source = *images[image_index];
	}
	
	std::lock_guard<std::mutex> thread_lock(thread_key);
	if (thread_slots.empty()) {
		thread_index = threads.size();
		threads.push_back(new std::thread(load_buffer_async, buffer_address, source, (int)buffer_id, thread_index, image_index));
	}
	else {
		thread_index = thread_slots.back();
		thread_slots.pop_back();
		threads[thread_index] = new std::thread(load_buffer_async, buffer_address, source, (int)buffer_id, thread_index, image_index);
	}
	return thread_index;
}

//Loads image data into a buffer.
gmx double load_image_data(char* buffer_address, double image_index) {
	int index = (int)image_index;
	image source;

	// Create a new scope to control image_lock lifetime.
	{
		std::lock_guard<std::mutex> image_lock(image_key);
		if (images.size() <= index || images[index] == NULL)
			return -1;
		source = *images[index];
	}
	load_buffer(buffer_address, source);
	return 1;
}

//Destroys a loaded image. Make sure to call this, otherwise you will cause a memory leak.
gmx double destroy_image(double image_index) {
	int index = (int)image_index;
	std::lock_guard<std::mutex> lock(image_key);
	if (images.size() <= index || images[index] == NULL)
		return 0;
	delete images[index];
	images[index] = NULL;
	image_slots.push_back(index);
	return 1;
}

//If the thread exists, detaches it.
gmx double cancel_thread(double thread_index) {
	std::lock_guard<std::mutex> locK(thread_key);
	return unsafe_thread_detach((int)thread_index);
}

gmx const char* get_error_message(double error) {
	return lodepng_error_text(error);
}
//Initializes GM interop functions. Should not be called by users.
gmx double RegisterCallbacks(char* arg1, char* arg2, char* arg3, char* arg4) {
	gml_event_perform_async = (void(*)(ds_map, int))arg1;
	gml_ds_map_create = (int(*)(int, ...))arg2;
	gml_ds_map_add_double = (bool(*)(ds_map, const char*, double))arg3;
	gml_ds_map_add_string = (bool(*)(ds_map, const char*, const char*))arg4;
	return 0;
}

