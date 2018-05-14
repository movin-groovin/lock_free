#ifndef __HAZARD_POINTERS_STACK_HPP__
#define __HAZARD_POINTERS_STACK_HPP__

#include <cstdint>

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
        typename HpManager = hp_manager<
            MaxThreadsNumber,
            lock_free::hp_node<T>,
            std::allocator<T>,
            basic_backoff
        >,
        typename Tag = void // for creating different objects of the same T
    > class stack: boost::noncopyable
    {
    public:
        static_assert(
            std::is_trivially_copyable<T>::value,
            "T must be trivially copyable"
        );

        static constexpr uint64_t MAX_THREADS_NUMBER = MaxThreadsNumber;

        using value_type = T;
        using node_type = hp_node<value_type>;
        using hp_manager_type = HpManager;
        using allocator_type = typename hp_manager_type::allocator_type;
        using backoff_strategy_type =
            typename hp_manager_type::backoff_strategy_type;

    public:
        stack(): m_thread_index_calculator(0), m_head(nullptr) {}
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

        bool push(const value_type& val)
        {
            uint64_t thread_index = get_thread_index();
            auto new_node = m_hpm.get_node(thread_index, val);

            while(true)
            {
                auto head = m_head.load(std::memory_order_consume);
                m_hpm.set_hp(thread_index, 0, head);
                if(head != m_head.load(std::memory_order_release))
                {
                    continue;
                }
                new_node->next.store(head, std::memory_order_consume);
                if(m_head.compare_exchange_weak(
                    head,
                    new_node,
                    std::memory_order_acq_rel
                )) break;

                m_backoff.wait();
            }
            m_hpm.set_hp(thread_index, 0, nullptr);

            return true;
        }

        bool pop(value_type& val)
        {
            node_type* head = nullptr;
            auto thread_index = get_thread_index();

            while(true)
            {
                head = m_head.load(std::memory_order_consume);
                m_hpm.set_hp(thread_index, 0, head);
                if(head != m_head.load(std::memory_order_release))
                {
                    continue;
                }
                auto next = head->next.load(std::memory_order_consume);
                if(!next)
                {
                    m_hpm.set_hp(thread_index, 0, nullptr);
                    return false;
                }
                val = head->value;
                if(m_head.compare_exchange_strong(
                    head,
                    next,
                    std::memory_order_acq_rel
                )) break;

                m_backoff.wait();
            }
            m_hpm.set_hp(thread_index, 0, nullptr);
            m_hpm.remove_node(thread_index, head);

            return true;
        }

    private:
        std::atomic<uint64_t> m_thread_index_calculator;
        std::atomic<node_type*> m_head;
        hp_manager_type m_hpm;
        backoff_strategy_type m_backoff;
    };
    //
}
}

#endif // __HAZARD_POINTERS_STACK_HPP__








