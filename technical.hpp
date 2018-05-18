#ifndef __TECHNICAL_LOCK_FREE_HPP__
#define __TECHNICAL_LOCK_FREE_HPP__

#include <cstdint>
#include <cassert>

#include <atomic>
#include <random>
#include <memory>
#include <utility>
#include <iterator>
#include <algorithm>
#include <array>
#include <vector>
#include <thread>
#include <chrono>
#include <type_traits>
#include <tuple>

#include <boost/noncopyable.hpp>



namespace lock_free
{
	//
    template <typename F>
    class scope_exit
    {
    public:
        scope_exit(F&& f): m_func(std::forward<F>(f)) {}
        ~scope_exit() { if(m_run) m_func(); }
        void set(bool val) {m_run = val;}
    private:
        F m_func;
        bool m_run = true;
    };

    template <typename F>
    scope_exit<F> make_scope_exit(F&& f)
    {
        return scope_exit<F>(std::forward<F>(f));
    }


    class basic_backoff
    {
    private:
        static constexpr uint64_t BASIC_FACTOR = 50;
        static constexpr uint64_t K = 2;

    public:
        basic_backoff(uint64_t max = 256 * BASIC_FACTOR): m_cnt_max(max) {}

        void wait() const
        {
            static thread_local uint64_t cnt = BASIC_FACTOR;
            
			uint64_t n = cnt * K;
			cnt = (n <= m_cnt_max ? n : BASIC_FACTOR);
            for (uint64_t i = 0; i < cnt; ++i)
            {
#ifdef __x86_64__
                __asm__("pause"); // nop
#else
                ;
#endif
            }
        }

    public:
        const uint64_t m_cnt_max;
    };

    class wait_backoff
    {
    public:
        void wait() const
        {
            //static thread_local std::default_random_engine rnd;
            //static thread_local std::uniform_int_distribution<uint64_t> distr(1, 500);
            static constexpr uint64_t BASIC_FACTOR = 50;
            
            std::this_thread::sleep_for(
                std::chrono::nanoseconds(/*distr(rnd)*/ BASIC_FACTOR)
            );
            //std::this_thread::yield(); // very poor approach
        }
    };

    template <typename Tag = void>
    class random_backoff
    {
    public:
        using tag_type = Tag;

        void wait() const
        {
			thread_local static std::default_random_engine rnd;
			thread_local static std::uniform_int_distribution<uint64_t> distr(1, 1000);
            
            for (uint64_t i = 0, j = distr(rnd); i < j; ++i);
        }
    };

    class empty_backoff
    {
    public:
        void wait() const {}
    };


    using tagged_pointer = void*;

    struct tptrs
    {
        static constexpr uint64_t PTR_BITS = 48;
        static constexpr uint64_t CNT_BITS = 16;
        static constexpr uint64_t TOTAL_BITS = PTR_BITS + CNT_BITS;
        // at 64 bit appropriate aligned pointers 3 least bits are zero
        static constexpr uint64_t CLEAR_INFO_BITS = ~0x7;

        static void* get_pointer(tagged_pointer val, bool clear_info_bits)
        noexcept
        {
            auto tmp = reinterpret_cast<uint64_t>(val) & 0x0000FFFFFFFFFFFF;
            if(clear_info_bits) tmp &= CLEAR_INFO_BITS;
            return reinterpret_cast<void*>(tmp);
        }
        template <typename T>
        static T get_pointer(tagged_pointer val, bool clear_info_bits = false)
        noexcept
        {
            static_assert(
                std::is_pointer<T>::value,
                "T must be pointer type"
            );
            return static_cast<T>( get_pointer(val, clear_info_bits) );
        }
        static void set_pointer(tagged_pointer& val, void* ptr) noexcept
        {
            val = reinterpret_cast<tagged_pointer>(
                (reinterpret_cast<uint64_t>(val) & 0xFFFF000000000000) |
                (reinterpret_cast<uint64_t>(ptr) & 0x0000FFFFFFFFFFFF)
            );
        }
        static uint16_t get_counter(tagged_pointer val) noexcept
        {
            return static_cast<uint16_t>(
                reinterpret_cast<uint64_t>(val) >> PTR_BITS
            );
        }
        static void set_counter(tagged_pointer& val, uint16_t cnt) noexcept
        {
            val = reinterpret_cast<tagged_pointer>(
                (static_cast<uint64_t>(cnt) << PTR_BITS) |
                (reinterpret_cast<uint64_t>(val) & 0x0000FFFFFFFFFFFF)
            );
        }
        static tagged_pointer set(void* ptr, uint16_t cnt) noexcept
        {
            return reinterpret_cast<tagged_pointer>(
                (reinterpret_cast<uint64_t>(ptr) & 0x0000FFFFFFFFFFFF) |
                (static_cast<uint64_t>(cnt) << PTR_BITS)
            );
        }
        static tagged_pointer increment(tagged_pointer val) noexcept
        {
            return set(get_pointer(val, false), get_counter(val) + 1);
        }
    };


    template <typename T>
    struct node
    {
        using value_type = T;

        node(): next(nullptr), value(value_type()) {}
        node(const value_type& val): next(nullptr), value(val) {}
        node(tagged_pointer ptr, const value_type& val):
            next(ptr),
            value(val)
        {}

        std::atomic<tagged_pointer> next;
        value_type value;
    };

    template <typename T>
    struct hp_node
    {
        using value_type = T;

        hp_node(): next(nullptr) {}
        hp_node(value_type const& ref): next(nullptr), value(ref) {}

        std::atomic<hp_node*> next;
        value_type value;
    };
    
    
    template <typename NodeType, typename BackOff>
    class stack_nodes_holder: boost::noncopyable
    {
    public:
        using node_type = NodeType;
        using value_type = typename NodeType::value_type;
        using backoff_strategy_type = BackOff;

        static_assert(
            std::is_trivially_copyable<value_type>::value,
            "value_type must be trivially copyable"
        );

        stack_nodes_holder(): m_head(nullptr) {}
        stack_nodes_holder(node_type* ptr): m_head(ptr)
        {
            assert(ptr->next == nullptr);
        }
        void init(node_type* ptr)
        {
            m_head.store(ptr, std::memory_order_relaxed);
            assert(ptr->next == nullptr);
        }
        /*
        node_type* extract_head()
        {
            auto head = m_head.load(std::memory_order_consume);
            assert(head != nullptr);
            m_head.store(nullptr, std::memory_order_release);
            return static_cast<node_type*>(head);
        }
        */
        node_type* get_node(void* ptr, const value_type& val)
        {
            auto node_ptr = get_node(val);
            if(!node_ptr) return nullptr;
            tptrs::get_pointer<node_type*>(node_ptr)->next.store(
                ptr, std::memory_order_relaxed
            );
            return node_ptr;
        }
        node_type* get_node(const value_type& val)
        {
            auto node_ptr = get_node();
            if(!node_ptr) return nullptr;
            tptrs::get_pointer<node_type*>(node_ptr)->value = val;
            return node_ptr;
        }
        node_type* get_node()
        {
            auto head = m_head.load(std::memory_order_consume);
            assert(head != nullptr);

            node_type* node_ptr = nullptr;
            while(true)
            {
                auto next = tptrs::get_pointer<node_type*>(head, true)->next.load(
                    std::memory_order_relaxed
                );
                if(!next) break;

                if (m_head.compare_exchange_strong(
                    head,
                    next,
                    std::memory_order_acq_rel
                )) {
                    head = tptrs::increment(head);
                    node_ptr = static_cast<node_type*>(head);
                    break;
                }
                m_backoff.wait();
            }

            return node_ptr;
        }

        bool save_node(node_type* ptr)
        {
            while(true)
            {
                auto head = m_head.load(std::memory_order_consume);
                assert(head != nullptr);

                tptrs::get_pointer<node_type*>(ptr, true)->next.store(
                    static_cast<node_type*>(head),
                    std::memory_order_relaxed
                );
                if(m_head.compare_exchange_strong(
                    head,
                    ptr,
                    std::memory_order_acq_rel
                )) break;
                m_backoff.wait();
            }

            return true;
        }

    private:
        std::atomic<tagged_pointer> m_head;
        backoff_strategy_type m_backoff;
    };

    template <typename NodeType, typename BackOff>
    class queue_nodes_holder: boost::noncopyable
    {
    public:
        using node_type = NodeType;
        using value_type = typename NodeType::value_type;
        using backoff_strategy_type = BackOff;

        static_assert(
            std::is_trivially_copyable<value_type>::value,
            "value_type must be trivially copyable"
        );

        queue_nodes_holder(): m_head(nullptr), m_tail(nullptr) {}
        queue_nodes_holder(node_type* ptr):
            m_head(ptr), m_tail(ptr)
        {
            assert(ptr->next == nullptr);
        }
        void init(node_type* ptr)
        {
            m_head.store(ptr, std::memory_order_relaxed);
            m_tail.store(ptr, std::memory_order_relaxed);
            assert(ptr->next == nullptr);
        }
        /*
        node_type* extract_head()
        {
            auto head = m_head.load(std::memory_order_consume);
            assert(head != nullptr);
            m_head.store(nullptr, std::memory_order_release);
            m_tail.store(nullptr, std::memory_order_release);
            return static_cast<node_type*>(head);
        }
        */
        node_type* get_node(void* ptr, const value_type& val)
        {
            auto node_ptr = get_node(val);
            if(!node_ptr) return nullptr;
            tptrs::get_pointer<node_type*>(node_ptr)->next.store(
                ptr, std::memory_order_relaxed
            );
            return node_ptr;
        }
        node_type* get_node(const value_type& val)
        {
            auto node_ptr = get_node();
            if(!node_ptr) return nullptr;
            tptrs::get_pointer<node_type*>(node_ptr)->value = val;
            return node_ptr;
        }
        node_type* get_node() // pop
        {
            while(true)
            {
                auto head = m_head.load(std::memory_order_consume);
                auto tail = m_tail.load(std::memory_order_consume);
                auto hnext = tptrs::get_pointer<node_type*>(head)->next.load(
                    std::memory_order_acquire
                );
                //if(head != m_head.load(std::memory_order_seq_cst)) continue;

                if (head == tail)
                {
                    if(!hnext)
                    {
assert( ((uint64_t)m_tail.load() & 0x4) == 0 );
                        return nullptr;
                    }
                    if(!m_tail.compare_exchange_strong(
                        tail, hnext, std::memory_order_release
                    )) m_backoff.wait();
assert( ((uint64_t)m_tail.load() & 0x4) == 0 );
                }
                else {
                    if(m_head.compare_exchange_strong(
                        head, hnext, std::memory_order_acq_rel
                    )) {
                        head = tptrs::increment(head);
assert( ((uint64_t)m_tail.load() & 0x4) == 0 );
                        return static_cast<node_type*>(head);
                    }
                    m_backoff.wait();
                }
                //
            }
            //
        }

        bool save_node(node_type* ptr) // push
        {
            tptrs::get_pointer<node_type*>(ptr)->next.store(
                nullptr, std::memory_order_seq_cst
            );
            while(true)
            {
                auto tail = m_tail.load(std::memory_order_consume);
                assert(tail != nullptr);
                auto next = tptrs::get_pointer<node_type*>(tail)->next.load(
                    std::memory_order_consume
                );
                //if(tail != m_tail.load(std::memory_order_acquire)) continue;

                if(!next)
                {
                    auto tail_ptr = tptrs::get_pointer<node_type*>(tail);
                    if(tail_ptr->next.compare_exchange_strong(
                        next,
                        ptr,
                        std::memory_order_acq_rel
                    )) {
                        m_tail.compare_exchange_strong(
                            tail, ptr, std::memory_order_release
                        );
assert( ((uint64_t)m_tail.load() & 0x4) == 0 );
                        break;
                    }
                    m_backoff.wait();
                }
                else {
                    if(!m_tail.compare_exchange_strong(
                        tail, next, std::memory_order_release
                    )) m_backoff.wait();
assert( ((uint64_t)m_tail.load() & 0x4) == 0 );
                }
                //
            }
assert( ((uint64_t)m_tail.load() & 0x4) == 0 );

            return true;
        }

    private:
        char padding1[128 - sizeof(std::atomic<tagged_pointer>)];
        std::atomic<tagged_pointer> m_head;
        char padding2[128 - sizeof m_head];
        std::atomic<tagged_pointer> m_tail;
        char padding3[128 - sizeof m_tail];
        backoff_strategy_type m_backoff;
    };


    template <typename Allocator>
    class allocator_holder
    {
    public:
        using allocator_type = Allocator;
        using node_type = typename Allocator::value_type;
        using value_type = typename node_type::value_type;

    public:
        node_type* allocate_and_construct()
        {
            auto ptr = m_allocator.allocate(1);
            m_allocator.construct(ptr);
            return ptr;
        }
        node_type* allocate_and_construct(value_type const& val)
        {
            auto ptr = m_allocator.allocate(1);
            // it's beter to use type_traits for check noexcept constructors
            try {
                m_allocator.construct(ptr, val);
            } catch(...) {
                m_allocator.deallocate(ptr, 1);
                throw;
            }
            return ptr;
        }
        void destroy_and_deallocate(node_type* ptr)
        {
            m_allocator.destroy(ptr);
            m_allocator.deallocate(ptr, 1);
        }

        allocator_type& get_allocator()
        {
            return m_allocator;
        }

    private:
        allocator_type m_allocator;
    };

	
    template<
        uint64_t MaxThreadsNumber,
        typename T,
        typename Allocator,
        typename BackOff = wait_backoff
    > class hp_manager: boost::noncopyable
    {
    public:
        static constexpr uint64_t MAX_THREADS_NUMBER = MaxThreadsNumber;
        static constexpr uint64_t K = 2;
        static constexpr uint64_t HP_NUM = 8;
        static constexpr uint64_t FREE_PTR_NUM = K * HP_NUM * MAX_THREADS_NUMBER;
        static constexpr uint64_t CURRENT_THREAD_ID = -1;

        using node_type = T;
        using value_type = typename T::value_type;
        using allocator_type =
            typename Allocator::template rebind<node_type>::other;
        using allocator_holder_type = allocator_holder<allocator_type>;
        using backoff_strategy_type = BackOff;

        struct thread_data_entry_type
        {
            std::array<std::atomic<node_type*>, HP_NUM> thread_hps = {};
            uint64_t free_ptrs_index = 0;
            std::array<node_type*, FREE_PTR_NUM> free_ptrs = {};
        };

    public:
        hp_manager() = default;
        ~hp_manager()
        {
            for(uint64_t i = 0; i < m_threads_number; ++i)
            {
                auto& thread_data = m_threads_data[i];
                auto& free_ptrs = thread_data.free_ptrs;
                for(uint64_t j = 0; j < thread_data.free_ptrs_index; ++j)
                {
                    physically_remove_node(free_ptrs[j]);
                }
            }
        }

        void thread_init(uint64_t /*thread_index*/) {}
        void init(
            uint64_t threads_number,
            uint64_t /*init_nodes_number*/,
            uint64_t /*max_nodes_number*/
        ) {
            m_threads_number = threads_number;
        }

        void set_hp(uint64_t thread_index, uint64_t pos, node_type* ptr)
        {
            auto& thread_data_entry = m_threads_data[thread_index];
            thread_data_entry.thread_hps[pos].store(
                ptr, std::memory_order_release
            );
        }
        node_type* get_hp(uint64_t thread_index, uint64_t pos)
        {
            auto& thread_data_entry = m_threads_data[thread_index];
            return thread_data_entry.thread_hps[pos].load(
                std::memory_order_acquire
            );
        }

        void remove_node(uint64_t thread_index, node_type* ptr)
        {
            auto& thread_data_entry = m_threads_data[thread_index];
            thread_data_entry.free_ptrs[thread_data_entry.free_ptrs_index++] = ptr;
            if(thread_data_entry.free_ptrs_index == FREE_PTR_NUM)
            {
                erase(thread_index);
            }
        }
        // if ptr wasn't in the chain (it is yet unused)
        void physically_remove_node(node_type* ptr)
        {
            m_allocator_holder.destroy_and_deallocate(ptr);
        }

        node_type* get_node(uint64_t /*thread_index*/)
        {
            return m_allocator_holder.allocate_and_construct();
        }
        node_type* get_node(uint64_t /*thread_index*/, const value_type& val)
        {
            auto ptr = get_node(uint64_t());
            ptr->value = val;
            return ptr;
        }

    private:
        void erase(uint64_t thread_index)
        {
            std::array<node_type*, HP_NUM * MAX_THREADS_NUMBER> hps;
            uint64_t total = 0;
            for(uint64_t i = 0; i < m_threads_number; ++i)
            {
                auto& thread_data = m_threads_data[i];

                for(uint64_t j = 0; j < HP_NUM; ++j)
                {
                    auto ptr = thread_data.thread_hps[j].load(
                        std::memory_order_consume
                    );
                    if (!ptr) break;
                    hps[total++] = ptr;
                }
            }

            // may be use radix sort
            auto begin = hps.data();
            auto end = begin + total;
            std::sort(begin, end);
            uint64_t busy_ptrs_cnt = 0;
            std::array<node_type*, HP_NUM * MAX_THREADS_NUMBER> busy_ptrs;
            auto& thread_data = m_threads_data[thread_index];

            // в free_ptrs самые свежие указатели в конце массива - optimize
            // свежие - т.е. могут быть в hazard pointers
            for(auto ptr : thread_data.free_ptrs)
            {
                assert(ptr != nullptr);
                if(std::binary_search(begin, end, ptr))
                {
                    busy_ptrs[busy_ptrs_cnt++] = ptr;
                    continue;
                }
                m_allocator_holder.destroy_and_deallocate(ptr);
            }
            std::copy(
                busy_ptrs.data(),
                busy_ptrs.data() + busy_ptrs_cnt,
                thread_data.free_ptrs.data()
            );
            thread_data.free_ptrs_index = busy_ptrs_cnt;
        }

    private:
        uint64_t m_threads_number = 0;
        allocator_holder_type m_allocator_holder;
        std::array<thread_data_entry_type, MAX_THREADS_NUMBER> m_threads_data;
    };
	//
}

namespace locked
{
    //
    template <typename BackOff = lock_free::empty_backoff>
    class spin_lock
    {
    public:
        using backoff_strategy_type = BackOff;

    public:
        spin_lock(): m_flag(false) {}
        void lock() const
        {
            while(true)
            {
                bool val = false;
                if(m_flag.compare_exchange_weak(
                    val,
                    true,
                    std::memory_order_acquire // r/r + r/w
                )) break;

                m_backoff.wait();
            }
        }
        void unlock() const
        {
            m_flag.store(false, std::memory_order_release);
        }

    private:
        mutable std::atomic<bool> m_flag;
        backoff_strategy_type m_backoff;
    };
    //
}

#endif // __TECHNICAL_LOCK_FREE_HPP__


















