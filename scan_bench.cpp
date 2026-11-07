#include <iostream>
#include <memory>
#include <numeric>
#include <vector>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cstdint>
#include <future>  
#include <bitset>
#include "qpl/qpl.h"

#include "utils.hpp"

constexpr const uint32_t VALUE_TO_FIND       = 48;
constexpr const uint32_t BIT_WIDTH           = 8;

void print_source_size(uint32_t size) {
    if (size >= (1024 * 1024 * 1024)) {
        std::cout << "Source Size: " << size / (1024 * 1024 * 1024) << " GB";
    } else if (size >= (1024 * 1024)) {
        std::cout << "Source Size: " << size / (1024 * 1024) << " MB";
    } else if (size >= 1024) {
        std::cout << "Source Size: " << size / 1024 << " KB";
    } else {
        std::cout << "Source Size: " << size << " Bytes";
    }
}

qpl_status benchmark_scan(qpl_path_t execution_path, uint8_t* source_array, char* destination_array, uint32_t size)
{
    uint32_t job_size = 0;
    qpl_status status = qpl_get_job_size(execution_path, &job_size);
    if (status != QPL_STS_OK) {
        std::cerr << "Error " << status << " while getting job size.\n";
        return status;
    }
    auto job_buffer = std::make_unique<uint8_t[]>(job_size);
    qpl_job* job = reinterpret_cast<qpl_job*>(job_buffer.get());
    status = qpl_init_job(execution_path, job);
    if (status != QPL_STS_OK) {
        std::cerr << "Error " << status << " while init job.\n";
        return status;
    }

    std::vector<uint8_t> dest_temp((size + 7) / 8);
    job->next_in_ptr        = source_array;
    job->available_in       = size;
    job->next_out_ptr       = dest_temp.data();
    job->available_out      = static_cast<uint32_t>(dest_temp.size());
    job->op                 = qpl_op_scan_eq;
    job->src1_bit_width     = BIT_WIDTH;
    job->num_input_elements = size;
    job->out_bit_width      = qpl_ow_nom;
    job->param_low          = VALUE_TO_FIND;
    job->flags              = QPL_FLAG_OUT_BE;

    status = qpl_execute_job(job);
    if (status != QPL_STS_OK) {
        std::cerr << "Error " << status << " while scan job.\n";
        return status;
    }
    // convert the mask output into char[]
    size_t remainder = size & (BIT_WIDTH - 1);
    size_t full_bytes = size / BIT_WIDTH;
    size_t output_offset = 0;
    for (size_t i = 0; i < full_bytes; ++i) {
        uint8_t num = static_cast<uint8_t>(dest_temp[i]);
        for (size_t bit = 0; bit < BIT_WIDTH; ++bit) {
            destination_array[output_offset++] =
                (num & (1u << (BIT_WIDTH - 1 - bit))) ? '1' : '0';
        }
    }
    if (remainder != 0) {
        uint8_t num = static_cast<uint8_t>(dest_temp[full_bytes]);
        for (size_t bit = 0; bit < remainder; ++bit) {
            destination_array[output_offset++] = (num & (1u << (BIT_WIDTH - 1 - bit))) ? '1' : '0';
        }
    }
    
    qpl_status fini_status = qpl_fini_job(job);
    if (status == QPL_STS_OK && fini_status != QPL_STS_OK) {
        status = fini_status;
        return status;
    }
    return status;
}

std::unique_ptr<char[]> benchmark_multithreaded(qpl_path_t execution_path, uint32_t source_size, int num_threads)
{
    std::vector<uint8_t> source_array(source_size);
    std::iota(source_array.begin(), source_array.end(), 0);
    source_array[source_size-1] = static_cast<uint8_t>(VALUE_TO_FIND);
    std::unique_ptr<char[]> destination_array(new char[source_size]{});

    auto start_time = std::chrono::steady_clock::now();
    std::vector<std::future<qpl_status>> futures;
    futures.reserve(num_threads);
    uint32_t base_chunk = source_size / num_threads;
    uint32_t remainder = source_size % num_threads;
        uint32_t offset = 0;
    for (uint32_t i = 0; i < static_cast<uint32_t>(num_threads); ++i)
    {
        uint32_t chunk_size = base_chunk + (i < remainder ? 1 : 0);
        uint8_t* source_part = source_array.data() + offset;
        char* destination_part = destination_array.get() + offset;
        futures.push_back(
            std::async(
                std::launch::async,
                benchmark_scan,
                execution_path,
                source_part,
                destination_part,
                chunk_size
            )
        );
        offset += chunk_size;
    }

    bool success = true;
    for (auto& f : futures) {
        qpl_status status = f.get();
        if (status != QPL_STS_OK)  success = false;
    }
    auto end_time = std::chrono::steady_clock::now();

    if (!success) {
        std::cerr << "One or more QPL jobs failed.\n";
        return nullptr;
    }

    double ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    double throughput_mbs = (static_cast<double>(source_size) * 1000.0) / (ms * 1024.0 * 1024.0);

    std::cout << "SCAN QPL..Threads: " << num_threads
              << ", Execution Time: " << ms << " ms"
              << ", Throughput: " << throughput_mbs << " MiB/s"
              << std::endl;
    
    return destination_array;
}

///////////////////////////////******************//////////////////////////////////////////
void linearSearch(const uint8_t* source_array, char* destination_array, uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i) destination_array[i] = (source_array[i] == VALUE_TO_FIND) ? '1' : '0';
}

std::unique_ptr<char[]> cpu_multithreaded(uint32_t source_size, int num_threads)
{
    std::vector<uint8_t> source_array(source_size);
    std::iota(source_array.begin(), source_array.end(), 0);
    source_array[source_size-1] = static_cast<uint8_t>(VALUE_TO_FIND);
    std::unique_ptr<char[]> destination_array(new char[source_size]{});
    
    auto start_time = std::chrono::steady_clock::now();
    std::vector<std::future<void>> futures;
    futures.reserve(num_threads);
    const uint32_t base_chunk = source_size / num_threads;
    const uint32_t remainder = source_size % num_threads;
    uint32_t offset = 0;
    for (uint32_t i = 0; i < static_cast<uint32_t>(num_threads); ++i)
    {
        uint32_t chunk_size = base_chunk + (i < remainder ? 1 : 0);
        uint8_t* source_part = source_array.data() + offset;
        char* destination_part = destination_array.get() + offset;

        futures.push_back(
            std::async(
                std::launch::async,
                linearSearch,
                source_part,
                destination_part,
                chunk_size
            )
        );
        offset += chunk_size;
    }
    for (auto& f : futures) f.get();
    auto end_time = std::chrono::steady_clock::now();
    
    double ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    double throughput_mbs = (static_cast<double>(source_size) * 1000.0) / (ms * 1024.0 * 1024.0);
    std::cout << "SCAN CPU..Threads: " << num_threads
              << ", Execution Time: " << ms << " ms"
              << ", Throughput: " << throughput_mbs << " MiB/s"
              << std::endl;
    
    return destination_array;
}

int main(int argc, char** argv) {
    std::cout << "Intel(R) Query Processing Library version is " << qpl_get_library_version() << ".\n";
    qpl_path_t execution_path = qpl_path_hardware;
    if (parse_execution_path(argc, argv, &execution_path, 0) != 0) {
        return 1;
    }

    // std::vector<uint32_t> source_sizes = {128 * 1024 * 1024};
    std::vector<uint32_t> source_sizes = {128 * 1024 * 1024, 256 * 1024 * 1024, 512 * 1024 * 1024, 1 * 1024 * 1024 * 1024};
    // std::vector<int> thread = {1, 2};
    std::vector<int> thread = {1, 2, 4, 8, 16, 32, 64, 96};
    
    for (uint32_t size : source_sizes) {
        for (auto num_threads: thread) {
            std::unique_ptr<char[]> cpu_result = cpu_multithreaded(size, num_threads);
            std::unique_ptr<char[]> qpl_result = benchmark_multithreaded(execution_path, size, num_threads);
            if (!cpu_result || !qpl_result) {
                std::cerr << "Benchmark failed.\n";
                return 1;
            }
            bool same = std::equal( cpu_result.get(), cpu_result.get() + size, qpl_result.get() );            
            std::cout << "Results identical: " << std::boolalpha << same << '\n';
        }
    }
    return 0;
}
