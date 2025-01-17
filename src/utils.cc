// © 2021 Lorian Richmond

#include <stdint.h>
#include "utils.hh"
#include "Extractor.hh"
#include "ui.hh"

#include <thread>
#include <zlib.h>

using namespace std;


namespace utils{

// Get the minimum number of bytes needed to store n bits
uint32_t bits_to_bytes(uint32_t num_bits){

	// Cast to float to avoid integer truncation
	return ceil((float)num_bits / 8);
}


// Use the first 24 bits generated by each seed to determine whether it is valid
void crack_seeds(const vector<bool> bits, uint32_t start_seed, uint32_t end_seed, uint32_t& progress_counter, vector<uint32_t>& valid_seeds){

	uint32_t seed = start_seed;

	do{

		Extractor e(bits, seed, true);

		if (e.check_magic() == true){

			valid_seeds.push_back(seed);
		}

		// Only update the progress counter every million seeds - this improves performance
		if ((seed-start_seed) % 1000000 == 999999){
			progress_counter += 1000000;
		}

	} while (++seed != end_seed);

	// Make sure the progress counter is left at the exact number done, not the previous million
	progress_counter = end_seed - start_seed;
}


// Find all seeds which generate valid magic bytes
vector<uint32_t> find_valid_seeds(const vector<bool>& bits, int num_threads){

	// Array to store the thread handles
	thread* threads[num_threads];

	// Array to store the number of seeds computed by each thread
	uint32_t thread_progress[num_threads];
	memset(thread_progress, 0, sizeof(thread_progress));

	// Array of vectors, each thread gets one vector in which to store its results
	vector<uint32_t> thread_results[num_threads];


	double seeds_per_thread = (double)UINT32_MAX / num_threads;

	for (int i=0; i<num_threads; i++){

		uint32_t start_seed = i * seeds_per_thread;
		uint32_t end_seed = (i+1) * seeds_per_thread;

		threads[i] = new thread(crack_seeds, bits, start_seed, end_seed, ref(thread_progress[i]), ref(thread_results[i]));
	}

	// Start a progress bar to inform the user of the threads' progress
	ui::create_progress_bar(thread_progress, num_threads);

	// Wait for all the threads to finish
	for (int i=0; i<num_threads; i++){

		threads[i]->join();
	}

	vector<uint32_t> found_seeds;

	// Merge the individual thread result vectors into a single vector
	for (int i=0; i<num_threads; i++){

		found_seeds.insert(found_seeds.end(), thread_results[i].begin(), thread_results[i].end());
	}

	return found_seeds;
}


// Shift 'num_bits' bits starting at address 'bytes' left by 1 bit
void shift_bits(uint8_t* bytes, uint64_t num_bits){

	// num_bits-1 as the last bit is discarded, it is moved into the bit before
	for (int i=0; i<num_bits-1; i++){

		int next_bit = (bytes[(i+1)/8] >> (i+1) % 8) & 1U;

		bytes[i/8] ^= (-next_bit ^ bytes[i/8]) & (1UL << i % 8);
	}
}


// Extract an embedded payload
void extract_payload(Extractor& e, ExtractedData& d){

	// The payload is stored as such: (<> indicates optional)
	// | is_compressed | <uncompressed_size> | has_checksum | <checksum> | filename | null byte | file contents |
	//      1 bit              32 bits            1 bit         32 bits    arbitrary    8 bits      arbitrary

	// If the payload is encrypted, we can only extract the encrypted data and exit
	if (d.is_encrypted){

		uint32_t num_enc_bytes = bits_to_bytes(d.info.payload_size);

		d.encrypted_payload.resize(num_enc_bytes);

		e.get_data(&d.encrypted_payload[0], d.info.payload_size);

		return;
	}

	// Check if the data is compressed
	e.get_data(&d.data.is_compressed, 1);

	uint8_t* payload;
	unsigned long payload_size_bytes;

	if (d.data.is_compressed){

		uint32_t num_compressed_bytes = bits_to_bytes(d.info.payload_size-1);

		// Get the size of the uncompressed payload
		e.get_data(&d.data.uncompressed_size, 32);
		payload_size_bytes = bits_to_bytes(d.data.uncompressed_size);

		// Extract the compressed payload
		uint8_t* compressed_payload = new uint8_t[num_compressed_bytes];
		e.get_data(compressed_payload, d.info.payload_size-1);

		// Uncompress the payload using zlib
		payload = new uint8_t[payload_size_bytes];
		uncompress (payload, &payload_size_bytes, compressed_payload, num_compressed_bytes);

		delete[] compressed_payload;

	}else{

		// If uncompressed, the payload can simply be extracted as-is
		payload_size_bytes = bits_to_bytes(d.info.payload_size - 1);
		payload = new uint8_t[payload_size_bytes];
		e.get_data(payload, d.info.payload_size - 1);
	}

	// Check if a checksum is embedded
	d.data.has_checksum = payload[0] & 1UL;

	// Shift the rest of the payload left by 1 bit to align bytes properly
	shift_bits(payload, payload_size_bytes*8);

	// If a checksum is present, extract it
	if(d.data.has_checksum){

		// Cast to uint32_t to get the first 4 bytes
		// Without casting, only 1 byte (sizeof char) is retrieved
		d.data.checksum = ((uint32_t*)payload)[0];
	}

	// If a checksum is embedded, the filename is offset 4 bytes to the right
	char* filename = (char*)payload + (d.data.has_checksum ? 4 : 0);

	// Find the length of the filename (it is terminated by a null byte so strlen can be used)
	uint32_t filename_length = (uint32_t)strlen(filename);

	// The start position of the file content (add 1 to account for the separating null byte)
	uint8_t* contents = (uint8_t*)filename + filename_length + 1;

	// The length of file content in bytes (-2 to account for checksum and compression bits not being included)
	uint32_t content_length = (uint32_t)(payload + bits_to_bytes(d.info.payload_size-2) - contents);

	d.data.filename = string(filename);
	d.data.file_contents.assign(contents, contents+content_length);

	delete[] payload;
}


// Further filter the possible seeds, and fully extract any that remain
vector<ExtractedData> extract_files(const vector<bool>& bits, const vector<uint32_t> seeds){

	// The embedded file metadata is stored as such:
	// | magic bytes | version | encryption algorithm | encryption mode | payload size (bits) | [payload]
	//     24 bits      1 bit           5 bits              3 bits              32 bits

	vector<ExtractedData> found_data;

	// The maximum possible size of the payload
	// (divided by 3 because each bit is stored in 3 DCT coefficients)
	// (the size of the metadata, 65, is taken away)
	const uint32_t max_payload_size = (uint32_t)(bits.size() / 3) - 65;

	for (int i=0, s=(int)seeds.size(); i<s; i++){

		Extractor e(bits, seeds[i], false);
		ExtractedData d;

		e.get_data(&d.info.magic_bytes, 24);
		e.get_data(&d.info.version, 1);
		e.get_data(&d.info.enc_algo, 5);
		e.get_data(&d.info.enc_mode, 3);
		e.get_data(&d.info.payload_size, 32);

		d.is_encrypted = (d.info.enc_algo != 0);

		// If the metadata doesn't make sense, it is not a real embedded file so is discarded
		if (d.info.payload_size > max_payload_size || d.info.enc_algo > 22 || d.info.version != 0){

			continue;
		}

		extract_payload(e, d);

		found_data.push_back(d);
	}

	return found_data;
}

}
