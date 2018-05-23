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
#include <stdexcept>

#include <boost/noncopyable.hpp>

#include "../technical.hpp"



namespace lock_free
{
namespace hp
{
    //
    template <typename T, typename Hash>
    struct basic_compare
    {
        using value_type = T;
        using hash_type = Hash;

        bool more_equal(const value_type& v1, const value_type& v2) const
        {
            return m_hash(v1) >= m_hash(v2);
        }
        bool equal(const value_type& v1, const value_type& v2) const
        {
            return m_hash(v1) == m_hash(v2);
        }

        hash_type m_hash;
    };

    template <typename T>
    struct hash_node
    {
        using value_type = T;

        hash_node(): next(nullptr) {}
        hash_node(value_type const& ref): next(nullptr), value(ref) {}
        void set_nodes_holder(bool val) {nodes_holder = val;}
        bool get_nodes_holder() const {return nodes_holder;}
        void set_counter(uint16_t val) {counter = val;}
        uint16_t get_counter() const {return counter;}

        std::atomic<hash_node*> next;
        value_type value;
        bool is_sentinel = false;
        bool nodes_holder = false;
        uint16_t counter = -1;
    };

    template<
        typename T,
        typename Cmp,
        typename BackOff,
        typename HpManager
    > class hash_flist: boost::noncopyable
    {
    public:
        static_assert(
            std::is_trivially_copyable<T>::value,
            "T must be trivially copyable"
        );

        static constexpr uint64_t REMOVED_MARK = 0x1;

        using value_type = T;
        using compare_type = Cmp;
        using hp_manager_type = HpManager;
        using node_type = typename hp_manager_type::node_type;
        using allocator_type = typename hp_manager_type::allocator_type;
        using backoff_strategy_type =
            typename hp_manager_type::backoff_strategy_type;

        struct find_result
        {
            node_type* prev = nullptr;
            node_type* curr = nullptr;
        };

    public:
        hash_flist(): m_head(nullptr) {}
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

        void thread_init(uint64_t thread_index)
        {
            m_hpm.thread_init(thread_index);
        }

        void init(
            uint64_t threads_number,
            uint64_t init_nodes_number = 0,
            uint64_t max_nodes_number = 0
        ) {
            m_hpm.init(
                threads_number,
                init_nodes_number,
                max_nodes_number
            );
            m_head.store(m_hpm.get_node(0), std::memory_order_relaxed);
        }

        node_type* add_sentinel(
            const value_type& val, node_type* start_node = nullptr
        ) {
            if(!start_node) start_node = m_head.load(std::memory_order_relaxed);
            while(auto ptr = start_node->next.load(std::memory_order_relaxed))
            {
                start_node = ptr;
            }
            //m_hpm.get_node(uint64_t(), val);
            node_type* new_node = m_hpm.physically_create_node();
            new_node->value = val;
            new_node->is_sentinel = true;
            start_node->next.store(new_node, std::memory_order_relaxed);
            return new_node;
        }

        bool contains(
            uint64_t thread_index,
            const value_type& val,
            node_type* start_node
        ) {
            bool ret{};

            auto res = search(thread_index, val, start_node);
            if(
                res.curr &&
                !res.curr->is_sentinel &&
                m_cmp.equal(res.curr->value, val)
            ) {
                ret = true;
            }
            m_hpm.set_hp(thread_index, 0, nullptr);
            m_hpm.set_hp(thread_index, 1, nullptr);
            //m_hpm.set_hp(thread_index, 2, nullptr);

            return ret;
        }

        node_type* add(
            uint64_t thread_index,
            const value_type& val,
            bool is_sentinel,
            node_type* start_node
        ) {
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
                auto res = search(thread_index, val, start_node);
                if(
                    res.curr &&
                    !res.curr->is_sentinel &&
                    m_cmp.equal(res.curr->value, val)
                ) {
                    m_hpm.physically_remove_node(new_node);
                    return nullptr;
                }

                new_node->next.store(res.curr, std::memory_order_relaxed);
                if(res.prev->next.compare_exchange_strong(
                    res.curr, new_node, std::memory_order_acq_rel
                )) return new_node;
                m_backoff.wait();
            }
            //
        }

        bool remove(
            uint64_t thread_index,
            const value_type& val,
            node_type* start_node
        ) {
            auto clear = make_scope_exit(
                [this, thread_index] () {
                    m_hpm.set_hp(thread_index, 0, nullptr);
                    m_hpm.set_hp(thread_index, 1, nullptr);
                    //m_hpm.set_hp(thread_index, 2, nullptr);
                }
            );

            while(true)
            {
                auto res = search(thread_index, val, start_node);

                if(
                    !res.curr ||
                    res.curr->is_sentinel ||
                    !m_cmp.equal(res.curr->value, val)
                ) {
                    return false;
                }
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

        find_result search(
            uint64_t thread_index,
            const value_type& val,
            node_type* start_node
        ) {
            node_type* prev{}, *curr{}, *next{};

            AGAIN:
            prev = start_node;
            assert( prev->is_sentinel );
            m_hpm.set_hp(thread_index, 0, prev);
            curr = prev->next.load(std::memory_order_consume);
            assert( !is_marked(curr) );
            m_hpm.set_hp(thread_index, 1, curr);
            if(curr != prev->next.load(std::memory_order_acquire)) goto AGAIN;
            while(true)
            {
                if(!curr) return find_result{prev, nullptr};
                else if(curr->is_sentinel) return find_result{prev, curr};
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
                if(curr->is_sentinel) return find_result{prev, curr};
                if(m_cmp.more_equal(curr->value, val))
                {
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
        std::atomic<node_type*> m_head;
        char padding1[128 - sizeof m_head];
        hp_manager_type m_hpm;
        compare_type m_cmp;
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
            load_factor_controller_type(): full(false) {}

            void increment(uint64_t thread_index)
            {
                dat[thread_index].cnt.fetch_add(1, std::memory_order_acquire);
            }
            void decrement(uint64_t thread_index)
            {
                dat[thread_index].cnt.fetch_sub(1, std::memory_order_acquire);
            }
            int64_t get_sum() const
            {
                int64_t sum = 0;
                for(uint64_t i = 0; i < MAX_THREADS_NUMBER; ++i)
                {
                    sum += dat[i].cnt.load(std::memory_order_consume);
                }
                return sum;
            }

            struct entry_type
            {
                entry_type(): cnt(0) {}

                std::atomic<int64_t> cnt;
                char padding[128 - sizeof cnt];
            };
            std::array<entry_type, MAX_THREADS_NUMBER> dat;
            std::atomic<bool> full;
        };

        using compare_type = basic_compare<T, Hash>;

        using hash_flist_type = hash_flist<
            T,
            compare_type,
            BackOff,
            hp_manager<
                MaxThreadsNumber,
                hash_node<T>,
                Allocator,
                BackOff
            >
        >;
        using value_type = T;
        using node_type = typename hash_flist_type::node_type;
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

        using data_type = std::array<node_type*, SIZE>;
        using backoff_strategy_type = BackOff;

    public:
        static_closed_hash_set(float load_factor = 2):
            m_load_factor(load_factor),
            m_thread_index_calculator(0),
            m_ptrs( std::make_unique<data_type>() )
        {}
        ~static_closed_hash_set() = default;

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
            m_data.thread_init( get_thread_index() );
        }

        void init(
            uint64_t init_nodes_number = 256 * 1024,
            uint64_t max_nodes_number = 0
        ) {
            m_data.init(
                m_thread_index_calculator.load(std::memory_order_relaxed),
                init_nodes_number,
                max_nodes_number
            );
            auto& ptrs = *m_ptrs;
            node_type* start_node = nullptr;
            for(uint64_t i = 0; i < SIZE; ++i)
            {
                start_node = m_data.add_sentinel(i, start_node);
                ptrs[i] = start_node;
            }
        }

        bool add(const value_type& value)
        {
//            if(
//                m_load_factor_controller.get_sum() + 1 >
//                static_cast<int64_t>(m_load_factor * SIZE)
//            ) {
//                throw std::runtime_error("insufficient resources");
//            }
            uint64_t thread_index = get_thread_index();
            auto bucket = m_hash(value) % SIZE;
            auto ret = m_data.add(thread_index, value, false, (*m_ptrs)[bucket]);
//            m_load_factor_controller.increment(thread_index);
            return ret;
        }

        bool remove(const value_type& value)
        {
            uint64_t thread_index = get_thread_index();
            auto bucket = m_hash(value) % SIZE;
            auto ret = m_data.remove(thread_index, value, (*m_ptrs)[bucket]);
//            m_load_factor_controller.decrement(thread_index);
            return ret;
        }

        bool contains(const value_type& value)
        {
            uint64_t thread_index = get_thread_index();
            auto bucket = m_hash(value) % SIZE;
            return m_data.contains(thread_index, value, (*m_ptrs)[bucket]);
        }

    private:
        float m_load_factor = 0;
        load_factor_controller_type m_load_factor_controller;
        std::atomic<uint64_t> m_thread_index_calculator;
        std::unique_ptr<data_type> m_ptrs;
        hash_flist_type m_data;
        hash_type m_hash;
    };
    //
}
}

#endif // __HAZARD_POINTERS_HASH_MAP_HPP__











