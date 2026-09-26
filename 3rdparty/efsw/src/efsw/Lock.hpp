#ifndef EFSW_LOCK_HPP
#define EFSW_LOCK_HPP

#include <efsw/Mutex.hpp>
#include <mutex>

namespace efsw {
using Lock = std::unique_lock<Mutex>;
} // namespace efsw

#endif
