#ifndef VITARECOMP_GPU_TEST_ONLY_NULL_BACKEND_HPP
#define VITARECOMP_GPU_TEST_ONLY_NULL_BACKEND_HPP

#include "gpu_internal.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vitarecomp::gpu::test {

class TestOnlyNullState {
public:
    void record(std::string event);
    [[nodiscard]] std::vector<std::string> events() const;
    void clear();

private:
    mutable std::mutex mutex_;
    std::vector<std::string> events_;
};

std::shared_ptr<BackendFactory> make_test_only_null_backend_factory(
    std::shared_ptr<TestOnlyNullState> state);

} // namespace vitarecomp::gpu::test

#endif
