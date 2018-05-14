#ifndef __TAGGED_POINTERS_STACK_HPP__
#define __TAGGED_POINTERS_STACK_HPP__

#include <cstdint>

#include <atomic>
#include <utility>
#include <memory>
#include <array>
#include <stdexcept>
#include <type_traits>

#include <boost/noncopyable.hpp>

#include "../technical.hpp"



namespace lock_free
{
namespace tp
{
    //
    template <
        uint64_t MaxThreadsNumber,
        uint64_t BucketsNumber,
        typename T,
        typename BackOff,
        typename Allocator = std::allocator<T>,
        typename Tag = void
    > class stack: private boost::noncopyable
    {
    public:
        static_assert(
            std::is_trivially_copyable<T>::value,
            "T must be trivially copyable"
        );

        static constexpr uint64_t MAX_THREADS_NUMBER = MaxThreadsNumber;
        static constexpr uint64_t BUCKETS_NUMBER = BucketsNumber;
        static constexpr uint64_t INFINITE_NUMBER = -1;

        using value_type = T;
        using node_type = node<T>;
        using allocator_type =
            typename Allocator:: template rebind<node_type>::other;
        using allocator_holder_type = allocator_holder<allocator_type>;
        using free_nodes_type = stack_nodes_holder<node_type, BackOff>;
        using backoff_strategy_type = BackOff;

        struct bucket_type
        {
            bucket_type(): current_nodes_number(0) {}

            std::atomic<uint64_t> current_nodes_number;
            uint64_t max_nodes_number = 0;
            free_nodes_type nodes_holder;
        };
        struct perthread_data_type
        {
            uint64_t bucket_index = 0;
        };

    private:
        uint64_t get_thread_index()
        {
            thread_local static uint64_t thread_index =
                m_thread_index_calc.fetch_add(1, std::memory_order_acquire);
            return thread_index;
        }

    public:
        stack(
            size_t init_nodes_number = 0,
            size_t max_nodes_number = INFINITE_NUMBER
        ):
            m_thread_index_calc(0),
            m_head(m_allocator_holder.allocate_and_construct())
        {
            for (uint64_t i = 0; i < BUCKETS_NUMBER; ++i)
            {
                auto& bucket = m_buckets[i];
                bucket.current_nodes_number.store(
                    init_nodes_number,
                    std::memory_order_relaxed
                );
                if(max_nodes_number < init_nodes_number)
                    bucket.max_nodes_number = init_nodes_number;
                else
                    bucket.max_nodes_number = max_nodes_number;
                bucket.nodes_holder.init(
                    m_allocator_holder.allocate_and_construct()
                );
                for (uint64_t j = 0; j < init_nodes_number; ++j)
                {
                    bucket.nodes_holder.save_node(
                        m_allocator_holder.allocate_and_construct()
                    );
                }
            }
        }
        ~stack() = default;
        void thread_init()
        {
            if(m_thread_index_calc.load(std::memory_order_acquire) >=
               MAX_THREADS_NUMBER
            ) throw std::runtime_error("Too many threads");
            get_thread_index();
        }
        void init()
        {
            auto j = m_thread_index_calc.load(std::memory_order_relaxed);
            for (uint64_t i = 0; i < j; ++i)
            {
                m_thread_data[i].bucket_index = i;
            }
        }

        bool push(value_type const& value)
        {
            uint64_t bucket_index =
                m_thread_data[get_thread_index()].bucket_index++ % BUCKETS_NUMBER;
            auto& bucket = m_buckets[bucket_index];
            auto& nodes_holder = bucket.nodes_holder;

            tagged_pointer new_node = nodes_holder.get_node(value);
            if (!new_node)
            {
                if(bucket.current_nodes_number.fetch_add(
                        1, std::memory_order_acq_rel
                   ) >= bucket.max_nodes_number
                ) {
                    bucket.current_nodes_number.fetch_sub(
                        1, std::memory_order_relaxed
                    );
                    return false;
                }
                try {
                    new_node = m_allocator_holder.allocate_and_construct(value);
                } catch(...) {
                    bucket.current_nodes_number.fetch_sub(
                        1, std::memory_order_seq_cst
                    );
                    throw;
                }
            }
            tagged_pointer current_head = m_head.load(std::memory_order_consume);

            while(true)
            {
                tptrs::get_pointer<node_type*>(new_node)->next.store(
                    current_head, std::memory_order_relaxed
                );
                // ABA+
                if (m_head.compare_exchange_strong(
                          current_head, new_node, std::memory_order_acq_rel
                    )
                ) return true;
                m_backoff.wait();
            }
        }
        bool pop(value_type& value)
        {
            auto current_head = m_head.load(std::memory_order_relaxed);
            while(true)
            {
                auto next_head =
                    tptrs::get_pointer<node_type*>(current_head)->next.load(
                        std::memory_order_relaxed
                    );
                if (!next_head) return false;
                value = tptrs::get_pointer<node_type*>(current_head)->value;
                if(m_head.compare_exchange_strong(
                        current_head, next_head, std::memory_order_acq_rel
                   )
                ) break;
                m_backoff.wait();
            }

            uint64_t bucket_index =
                m_thread_data[get_thread_index()].bucket_index++ % BUCKETS_NUMBER;
            auto& bucket = m_buckets[bucket_index];
            bucket.nodes_holder.save_node(static_cast<node_type*>(current_head));

            return true;
        }

        uint64_t get_nodes_count(uint64_t bucket_index) const
        {
            return m_buckets[bucket_index].nodes_holder.get_nodes_count();
        }

    private:
        std::atomic<uint64_t> m_thread_index_calc;
        std::array<bucket_type, BUCKETS_NUMBER> m_buckets;
        char padding1[128 - sizeof m_buckets];
        std::array<perthread_data_type, MAX_THREADS_NUMBER> m_thread_data;
        allocator_holder_type m_allocator_holder;
        char padding2[128 - sizeof m_allocator_holder];
        std::atomic<tagged_pointer> m_head;
        char padding3[128 - sizeof m_head];
        backoff_strategy_type m_backoff;
    };
    //
}
}

#endif // __TAGGED_POINTERS_STACK_HPP__















