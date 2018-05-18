#ifndef __LOCKED_HASH_MAP_HPP__
#define __LOCKED_HASH_MAP_HPP__

#include <cstdint>

#include <mutex>
#include <vector>
#include <array>
#include <unordered_set>
#include <algorithm>

#include <boost/noncopyable.hpp>

#include "../technical.hpp"



namespace locked
{
    //
    template <
        typename T,
        uint64_t N = 1024,
        typename Lock = spin_lock<lock_free::wait_backoff>
    >
    class striped_unordered_set: boost::noncopyable
    {
    public:
        static constexpr uint64_t SIZE = N;
        using value_type = T;
        using lock_type = Lock;
        using entry_holder_data_type = std::unordered_set<value_type>;
        struct entry_holder_type
        {
            lock_type synch;
            entry_holder_data_type dat;
        };
        using hash_type = typename entry_holder_data_type::hasher;

    public:
        striped_unordered_set() = default;
        ~striped_unordered_set() = default;

        bool contains(const value_type& val)
        {
            auto bucket = m_hash(val) % SIZE;
            auto& bucket_ref = m_dat[bucket];
            std::lock_guard<lock_type> lck(bucket_ref.synch);

            return bucket_ref.dat.find(val) != bucket_ref.dat.end();
        }

        bool add(const value_type& val)
        {
            auto bucket = m_hash(val) % SIZE;
            auto& bucket_ref = m_dat[bucket];
            std::lock_guard<lock_type> lck(bucket_ref.synch);

            auto it = bucket_ref.dat.find(val);
            if(it != bucket_ref.dat.end()) return false;
            bucket_ref.dat.insert(it, val);
            return true;
        }

        bool remove(const value_type& val)
        {
            auto bucket = m_hash(val) % SIZE;
            auto& bucket_ref = m_dat[bucket];
            std::lock_guard<lock_type> lck(bucket_ref.synch);

            return static_cast<bool>(bucket_ref.dat.erase(val));
        }

    private:
        std::array<entry_holder_type, N> m_dat;
        hash_type m_hash;
    };
    //
}

#endif // __LOCKED_HASH_MAP_HPP__










