#ifndef __HAZARD_POINTERS_HASH_MAP_HPP__
#define __HAZARD_POINTERS_HASH_MAP_HPP__

#include <cstdint>
#include <cassert>

#include <atomic>
#include <memory>
#include <utility>
#include <vector>
#include <stdexcept>
#include <functional>
#include <type_traits>

#include <boost/noncopyable.hpp>

#include "../technical.hpp"



namespace lock_free
{
namespace hp
{
    //
    template <typename T>
    struct hash_node
    {
        using value_type = T;

        hash_node(): next(nullptr) {}
        hash_node(value_type const& ref): next(nullptr), value(ref) {}

        std::atomic<hp_node*> next;
        value_type value;
        bool is_sentinel = false;
    };

    template<
        uint64_t MaxThreadsNumber,
        typename T,
        typename BackOff = wait_backoff,
        typename HpManager = hp_manager<
            MaxThreadsNumber,
            hash_node<T>,
            std::allocator<T>,
            BackOff
        >,
        typename Tag // for creating different objects of the same T
    > class hash_flist: boost::noncopyable
    {
    public:
        static_assert(
            std::is_trivially_copyable<T>::value,
            "T must be trivially copyable"
        );

        static constexpr uint64_t MAX_THREADS_NUMBER = MaxThreadsNumber;
        static constexpr uint64_t REMOVED_MARK = 0x1;

        using value_type = T;
        using node_type = hash_node<value_type>;
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
        hash_flist(): m_thread_index_calculator(0), m_head(nullptr) {}
        ~hash_flist()
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

        bool add(const value_type& val, bool is_sentinel)
        {
            uint64_t thread_index = get_thread_index();
            auto new_node = m_hpm.get_node(thread_index, val);
            new_node->is_sentinel = is_sentinel;
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

        find_result search(const value_type& val, bool for_removing = false)
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
                // пропускаем постоянные узлы
                if(curr->value >= val &&
                   !(for_removing && curr->is_sentinel)
                ) {
                    return find_result{prev, curr};
                }

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

    template<
        uint64_t MaxThreadsNumber,
        uint64_t N,
        typename T,
        typename BackOff = empty_backoff,
        typename Hash = std::hash<T>,
        typename Allocator = std::allocator<T>,
        typename Tag = void // for creating different objects of the same T
    > class static_closed_hash_set: boost::noncopyable
    {
    public:
        static constexpr uint64_t MAX_THREADS_NUMBER = MaxThreadsNumber;
        static constexpr uint64_t SIZE = N;

        struct load_factor_controller_type
        {
            void increment(uint64_t thread_index)
            {
                dat[thread_index].cnt.fetch_add(1, std::memory_order_acquire);
            }
            void decrement(uint64_t thread_index)
            {
                dat[thread_index].cnt.fetch_sub(1, std::memory_order_acquire);
            }
            uint64_t get_sum() const
            {
                uint64_t sum = 0;
                for(uint64_t i = 0; i < MAX_THREADS_NUMBER; ++i)
                {
                    sum += dat[i].cnt.load(std::memory_order_consume);
                }
                return sum;
            }

            struct entry_type
            {
                entry_type(): cnt(0) {}

                std::atomic<uint64_t> cnt;
                char padding[128 - sizeof cnt];
            };
            std::array<entry_type, MAX_THREADS_NUMBER> dat;
        };

        using value_type = T;
        //using node_type = hash_set_node<T>;
        using hash_type = Hash;
        using hash_result_type = typename Hash::result_type;

        static_assert(
            std::is_same<hash_result_type, size_t>::value,
            "need size_t type as hash result type"
        );
        static_assert(
            std::is_trivially_copyable<value_type>::value,
            "value_type must be trivially copyable type"
        );

        using data_type = std::array<entry_type, SIZE>;
        using backoff_strategy_type =
            typename hp_manager_type::backoff_strategy_type;

    public:
        static_closed_hash_set(float load_factor = 0.75):
            m_load_factor(load_factor),
            m_max_elements(static_cast<uint64_t>(m_load_factor * SIZE)),
            m_thread_index_calculator(0),
            m_data( std::make_unique<data_type>() )
        {}
        ~static_open_hash_set()
        {
            for(auto& entry : *m_data)
            {
                if(auto ptr = entry.node.load(std::memory_order_relaxed))
                {
                    m_hpm.physically_remove_node(clear_mark(ptr));
                }
            }
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
        }

        bool add(const value_type& value)
        {
            uint64_t thread_index = get_thread_index();
            auto key = m_hash(value);
            uint64_t i = key % SIZE;
            uint64_t stop_i = i;
            node_type* new_node = nullptr;
            auto clear = make_scope_exit(
                [this, thread_index] () {
                    m_hpm.set_hp(thread_index, 0, nullptr);
                    //m_hpm.set_hp(thread_index, 1, nullptr);
                }
            );

            auto sum = m_load_factor_controller.get_sum();
            if(sum >= m_max_elements)
            {
                // resize
                return false;
            }

            do
            {
                auto& entry = (*m_data)[i];
                auto entry_node = entry.node.load(std::memory_order_acquire);
                if(entry_node && !is_marked(entry_node))
                {
                    m_hpm.set_hp(thread_index, 0, entry_node);
                    if(entry_node != entry.node.load(std::memory_order_acquire))
                    {
                        i = key % SIZE;
                        continue;
                    }
                    if(entry_node->value == value) return false;
                    //
                }
                else if( !entry_node || is_marked(entry_node) ) {
                    if(!new_node) new_node = m_hpm.get_node(thread_index, value);
                    if(entry.node.compare_exchange_strong(
                        entry_node, new_node, std::memory_order_release
                    )) {
                        m_load_factor_controller.increment(thread_index);
                        return true;
                    }
                    m_backoff.wait();
                }
                i = (i + 1) % SIZE;
            } while(i != stop_i);

            return false;
        }

        bool remove(const value_type& value)
        {
            uint64_t thread_index = get_thread_index();
            auto key = m_hash(value);
            uint64_t i = key % SIZE;
            uint64_t stop_i = i;
            auto clear = make_scope_exit(
                [this, thread_index] () {
                    m_hpm.set_hp(thread_index, 0, nullptr);
                    //m_hpm.set_hp(thread_index, 1, nullptr);
                }
            );

            do
            {
                auto& entry = (*m_data)[i];
                auto entry_node = entry.node.load(std::memory_order_acquire);
                if(!entry_node) return false;
                if(!is_marked(entry_node))
                {
                    m_hpm.set_hp(thread_index, 0, entry_node);
                    if(entry_node != entry.node.load(std::memory_order_acquire))
                    {
                        i = key % SIZE;
                        continue;
                    }
                    if(entry_node->value == value)
                    {
                        if(entry.node.compare_exchange_strong(
                            entry_node,
                            add_mark(nullptr),
                            std::memory_order_acq_rel
                        )) {
                            m_hpm.remove_node(thread_index, entry_node);
                            m_load_factor_controller.decrement(thread_index);
                            return true;
                        }
                        m_backoff.wait();
                    }
                }
                // if(is_marked(entry_node)) - logically removed, go to next
                i = (i + 1) % SIZE;
            } while(i != stop_i);

            return false;
        }

        bool contains(const value_type& value)
        {
            uint64_t thread_index = get_thread_index();
            auto key = m_hash(value);
            uint64_t i = key % SIZE;
            uint64_t stop_i = i;
            auto clear = make_scope_exit(
                [this, thread_index] () {
                    m_hpm.set_hp(thread_index, 0, nullptr);
                    //m_hpm.set_hp(thread_index, 1, nullptr);
                }
            );

            do
            {
                auto& entry = (*m_data)[i];
                auto entry_node = entry.node.load(std::memory_order_acquire);
                if(!entry_node) return false;
                if(!is_marked(entry_node))
                {
                    m_hpm.set_hp(thread_index, 0, entry_node);
                    if(entry_node != entry.node.load(std::memory_order_acquire))
                    {
                        i = key % SIZE;
                        continue;
                    }
                    if(entry_node->value == value) return true;
                    // else go to next
                }
                // if(is_marked(entry_node)) - logically removed, go to next
                i = (i + 1) % SIZE;
            } while(i != stop_i);

            return false;
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

    private:
        float m_load_factor = 0;
        uint64_t m_max_elements = 0;
        load_factor_controller_type m_load_factor_controller;
        std::atomic<uint64_t> m_thread_index_calculator;
        std::unique_ptr<data_type> m_data;
        hash_type m_hash;
        hp_manager_type m_hpm;
        backoff_strategy_type m_backoff;
    };
    //
}
}

#endif // __HAZARD_POINTERS_HASH_MAP_HPP__











