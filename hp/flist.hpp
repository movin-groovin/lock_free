#ifndef __HAZARD_POINTERS_FLIST_HPP__
#define __HAZARD_POINTERS_FLIST_HPP__

#include <cstdint>
#include <cassert>

#include <atomic>
#include <memory>
#include <utility>
#include <array>
#include <stdexcept>

#include <boost/noncopyable.hpp>

#include "../technical.hpp"



namespace lock_free
{
namespace hp
{
    //
    template<
        uint64_t MaxThreadsNumber,
        typename T,
        typename BackOff = wait_backoff,
        typename HpManager = hp_manager<
            MaxThreadsNumber,
            lock_free::hp_node<T>,
            std::allocator<T>,
            BackOff
        >,
        typename Tag = void // for creating different objects of the same T
    > class flist: boost::noncopyable
    {
    public:
        static_assert(
            std::is_trivially_copyable<T>::value,
            "T must be trivially copyable"
        );

        static constexpr uint64_t MAX_THREADS_NUMBER = MaxThreadsNumber;
        static constexpr uint64_t REMOVED_MARK = 0x1;

        using value_type = T;
        using node_type = hp_node<value_type>;
        using hp_manager_type = HpManager;
        using allocator_type = typename hp_manager_type::allocator_type;
        using backoff_strategy_type =
            typename hp_manager_type::backoff_strategy_type;

        struct find_result
        {
            node_type* prev = nullptr;
            node_type* curr = nullptr;
        };

    public:
        flist(): m_thread_index_calculator(0), m_head(nullptr) {}
        ~flist()
        {
            auto ptr = m_head.load(std::memory_order_consume);
            while(ptr)
            {
                auto next = ptr->next.load(std::memory_order_consume);
                next = reinterpret_cast<node_type*>(
                    reinterpret_cast<uint64_t>(next) & ~REMOVED_MARK
                );
                m_hpm.physically_remove_node(ptr);
                ptr = next;
            }
            m_head.store(nullptr, std::memory_order_relaxed);
        }

        uint64_t get_thread_index()
        {
            thread_local static uint64_t thread_index =
                m_thread_index_calculator.fetch_add(1, std::memory_order_acquire);
            return thread_index;
        }
        void thread_init()
        {
            if(m_thread_index_calculator.load(std::memory_order_acquire) >=
               MAX_THREADS_NUMBER
            ) throw std::runtime_error("Too many threads");
            m_hpm.thread_init( get_thread_index() );
        }
        void init(
            uint64_t init_nodes_number = 0,
            uint64_t max_nodes_number = 0
        ) {
            m_hpm.init(
                m_thread_index_calculator.load(std::memory_order_relaxed),
                init_nodes_number,
                max_nodes_number
            );
            m_head.store(m_hpm.get_node(0), std::memory_order_relaxed);
        }

        bool contains(const value_type& val)
        {
            bool ret{};
            uint64_t thread_index = get_thread_index();

            auto res = search(val);
            if(res.curr && res.curr->value == val) ret = true;
            m_hpm.set_hp(thread_index, 0, nullptr);
            m_hpm.set_hp(thread_index, 1, nullptr);
            //m_hpm.set_hp(thread_index, 2, nullptr);

            return ret;
        }

        bool add(const value_type& val)
        {
            uint64_t thread_index = get_thread_index();
            auto new_node = m_hpm.get_node(thread_index, val);
            auto clear = make_scope_exit(
                [this, thread_index] () {
                    m_hpm.set_hp(thread_index, 0, nullptr);
                    m_hpm.set_hp(thread_index, 1, nullptr);
                    //m_hpm.set_hp(thread_index, 2, nullptr);
                }
            );

            while(true)
            {
                auto res = search(val);
                if(res.curr && res.curr->value == val)
                {
                    m_hpm.physically_remove_node(new_node);
                    return false;
                }

                new_node->next.store(res.curr, std::memory_order_relaxed);
                if(res.prev->next.compare_exchange_strong(
                    res.curr, new_node, std::memory_order_acq_rel
                )) return true;
                m_backoff.wait();
            }
            //
        }

        bool remove(const value_type& val)
        {
            auto thread_index = get_thread_index();
            auto clear = make_scope_exit(
                [this, thread_index] () {
                    m_hpm.set_hp(thread_index, 0, nullptr);
                    m_hpm.set_hp(thread_index, 1, nullptr);
                    //m_hpm.set_hp(thread_index, 2, nullptr);
                }
            );

            while(true)
            {
                auto res = search(val);

                if(!res.curr || res.curr->value != val) return false;
                auto next = res.curr->next.load(std::memory_order_consume);
                if(is_marked(next)) continue;
                if(res.curr->next.compare_exchange_strong(
                    next, add_mark(next), std::memory_order_release
                )) {
                    // 1 attempt to remove physically
                    //   next and cleared_next may be already
                    //   logically deleted (with 0x1)
                    //res.prev->next.compare_exchange_strong(
                    //    res.curr, cleared_next, std::memory_order_release
                    //);
                    return true;
                }
                m_backoff.wait();
            }
            //
        }

    private:
        static bool is_marked(node_type* p)
        {
            return reinterpret_cast<uint64_t>(p) & REMOVED_MARK;
        }
        static node_type* clear_mark(node_type* p)
        {
            return reinterpret_cast<node_type*>(
                reinterpret_cast<uint64_t>(p) & ~REMOVED_MARK
            );
        }
        static node_type* add_mark(node_type* p)
        {
            return reinterpret_cast<node_type*>(
                reinterpret_cast<uint64_t>(p) | REMOVED_MARK
            );
        }

        find_result search(const value_type& val)
        {
            uint64_t thread_index = get_thread_index();
            node_type* prev{}, *curr{}, *next{};

            AGAIN:
            prev = m_head.load(std::memory_order_consume);
            m_hpm.set_hp(thread_index, 0, prev);
            curr = prev->next.load(std::memory_order_consume);
            assert( !is_marked(curr) );
            m_hpm.set_hp(thread_index, 1, curr);
            if(curr != prev->next.load(std::memory_order_acquire)) goto AGAIN;
            while(true)
            {
                if(!curr) return find_result{prev, nullptr};
                next = curr->next.load(std::memory_order_consume);
                while(is_marked(next))
                {
                    node_type* cleared_next = clear_mark(next);
                    if(!prev->next.compare_exchange_strong(
                        curr, cleared_next, std::memory_order_acq_rel
                    )) {
                        m_backoff.wait();
                        goto AGAIN;
                    }

                    assert( !is_marked(curr) );
                    m_hpm.remove_node(thread_index, curr);
                    if(!cleared_next) return find_result{prev, nullptr};
                    curr = cleared_next;
                    m_hpm.set_hp(thread_index, 1, curr);
                    if(curr != prev->next.load(std::memory_order_seq_cst))
                        goto AGAIN;
                    next = curr->next.load(std::memory_order_relaxed);
                }
                if(curr->value >= val) return find_result{prev, curr};

                prev = curr;
                m_hpm.set_hp(thread_index, 0, prev);
                curr = next;
                m_hpm.set_hp(thread_index, 1, curr);
                if(curr != prev->next.load(std::memory_order_seq_cst)) goto AGAIN;
            }
            //
        }

    private:
        std::atomic<uint64_t> m_thread_index_calculator;
        std::atomic<node_type*> m_head;
        char padding1[128 - sizeof m_head];
        hp_manager_type m_hpm;
        backoff_strategy_type m_backoff;
    };
    //
}
}

#endif // __HAZARD_POINTERS_FLIST_HPP__
