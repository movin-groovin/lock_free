#ifndef __LOCKED_QUEUE_HPP__
#define __LOCKED_QUEUE_HPP__

#include <cstdint>

#include <mutex>
#include <queue>
#include <array>

#include <boost/noncopyable.hpp>

#include "../technical.hpp"



namespace locked
{
	//
    template <uint64_t Number, typename T>
    class two_threads_queue
    {
    public:
        static constexpr uint64_t DATA_NUMBER = Number;

        using value_type = T;

    public:
        two_threads_queue() = default;

        bool push(value_type const& val)
        {
            if(m_data[i_push % DATA_NUMBER].load(std::memory_order_acquire) !=
               value_type()
            ) {
                return false;
            }
            auto previous_value = m_data[i_push % DATA_NUMBER].exchange(
                val, std::memory_order_release
            );
            ++i_push;

            return true;
        }

        bool pop(value_type& val)
        {
            auto previous_value = m_data[i_pop % DATA_NUMBER].exchange(
                value_type(), std::memory_order_release
            );
            if(previous_value == value_type())
            {
                return false;
            }
            ++i_pop;
            val = previous_value;

            return true;
        }

    private:
        std::array<std::atomic<value_type>, DATA_NUMBER> m_data = {};
        char padding1[128 - sizeof(value_type)];
        uint64_t i_push = 0;
        char padding2[128 - sizeof i_push];
        uint64_t i_pop = 0;
        char padding3[128 - sizeof i_pop];
    };


    template <
        typename T,
        typename Lock
    > class locked_queue: boost::noncopyable
    {
    private:
        using value_type = T;
        using lock_type = Lock;

    public:
        locked_queue() = default;

        bool push(const value_type& value)
        {
            std::lock_guard<lock_type> lck(m_synch);
            m_data.push(value);
            return true;
        }

        bool pop(value_type& value)
        {
            std::lock_guard<lock_type> lck(m_synch);
            if (m_data.empty()) return false;
            value = std::move( m_data.front() );
            m_data.pop();
            return true;
        }

    private:
        std::queue<value_type> m_data;
        lock_type m_synch;
    };
	//
}

#endif // __LOCKED_QUEUE_HPP__
