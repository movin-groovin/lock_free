
#include <iostream>
#include <string>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <future>
#include <utility>
#include <random>

#include <hp/hash_set.hpp>
#include <locked/hash_set.hpp>



namespace tools
{
    template<typename T = size_t>
    struct random_uniformly_gen
    {
    public:
        random_uniformly_gen(T min, T max): m_min(min), m_max(max), m_distributor(m_min, m_max) {}

        size_t operator()() const
        {
            return m_distributor(m_random_engine);
        }

    private:
        T m_min = 0;
        T m_max = 1;
        mutable std::default_random_engine m_random_engine;
        mutable std::uniform_int_distribution<T> m_distributor;
    };


    template <typename T, typename ... Args>
    auto& get_structure(Args && ... args)
    {
        static T obj{std::forward<Args>(args)...};
        return obj;
    }


    struct stat_data
    {
        size_t success_producer = 0;
        size_t success_consumer = 0;
        size_t fail_producer = 0;
        size_t fail_consumer = 0;
        size_t max_prod_nsec=0;
        size_t max_cons_nsec=0;
        size_t min_prod_nsec=-1;
        size_t min_cons_nsec=-1;
        size_t nsec_total = 0;
        size_t call_count = 0;
        size_t average_prod_nsec=0;
        size_t average_cons_nsec=0;
    };


    struct results_data
    {
        stat_data stat;
        std::future<void> fut;
        char padding[128 - (sizeof stat + sizeof fut)];
    };

    template <typename T>
    struct need_init
    {
        static void init(T& ref) {}
        static void thread_init(T& ref) {}
    };
    // for hp flist
    template <
        template<uint64_t, uint64_t, typename ...> class T,
        uint64_t N,
        uint64_t M,
        typename ... Args
    > struct need_init< T<N, M, Args...> >
    {
        using tmpl_type = T<N, M, Args...>;
        static void init(tmpl_type& ref) { ref.init(); }
        static void thread_init(tmpl_type& ref) { ref.thread_init(); }
    };
}


int main(int /*argc*/, char** /*argv*/)
{
    using namespace tools;

    lock_free::hp::static_closed_hash_set<
        8, 1024 * 1024, size_t
    > structure(2);
//    locked::striped_unordered_set<size_t, 1024 * 4> structure;

    constexpr size_t WAIT_NUM = 10;
    constexpr size_t prod_thread_num = 4;
    constexpr size_t cons_thread_num = 4;
    constexpr size_t thread_num = prod_thread_num + prod_thread_num;
    results_data prod_arr[prod_thread_num];
    results_data cons_arr[cons_thread_num];
    std::atomic<bool> start(false);
    std::atomic<bool> stop(false);
    std::atomic<size_t> started_num(0);

    auto prod_func =
        [&structure, &prod_arr, &start, &stop, &started_num]
        (size_t i) mutable -> void
        {
            need_init<decltype(structure)>::thread_init(structure);
            random_uniformly_gen<size_t> rgen(1, 2 * 1024 * 1024);

            ++started_num;
            while(!start);
            while (!stop)
            {
                auto val = rgen();
                auto ts1 = std::chrono::high_resolution_clock::now();
                bool res = structure.add(val);
                auto ts2 = std::chrono::high_resolution_clock::now();
                size_t nsec_latency =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        ts2 - ts1
                    ).count();

                auto& stat = prod_arr[i].stat;
                if (res) ++stat.success_producer;
                else ++stat.fail_producer;
                if (nsec_latency > stat.max_prod_nsec)
                    stat.max_prod_nsec = nsec_latency;
                if (nsec_latency < stat.min_prod_nsec)
                    stat.min_prod_nsec = nsec_latency;
                stat.nsec_total += nsec_latency;
                ++stat.call_count;
            }
        };
    auto cons_func =
        [&structure, &cons_arr, &start, &stop, &started_num]
        (size_t i) mutable -> void
        {
            random_uniformly_gen<size_t> rgen(1, 2 * 1024 * 1024);
            need_init<decltype(structure)>::thread_init(structure);

            ++started_num;
            while(!start);
            while (!stop)
            {
                auto ts1 = std::chrono::high_resolution_clock::now();
                auto res = structure.remove(rgen());
                auto ts2 = std::chrono::high_resolution_clock::now();
                size_t nsec_latency =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        ts2 - ts1
                    ).count();

                auto& stat = cons_arr[i].stat;
                if (res) ++stat.success_consumer;
                else ++stat.fail_consumer;
                if (nsec_latency > stat.max_cons_nsec)
                    stat.max_cons_nsec = nsec_latency;
                if (nsec_latency < stat.min_cons_nsec)
                    stat.min_cons_nsec = nsec_latency;
                stat.nsec_total += nsec_latency;
            }
        };

    for (size_t i = 0; i < prod_thread_num; ++i)
    {
        auto f = [i, prod_func] () mutable {prod_func(i);};
        prod_arr[i].fut = std::async(std::launch::async, f);
    }
    for (size_t i = 0; i < cons_thread_num; ++i)
    {
        auto f = [i, cons_func] () mutable {cons_func(i);};
        cons_arr[i].fut = std::async(std::launch::async, f);
    }
    while (started_num < thread_num);
    need_init<decltype(structure)>::init(structure);
    start = true;
    std::this_thread::sleep_for( std::chrono::seconds(WAIT_NUM) );
    stop = true;
    for (auto& ref : prod_arr) ref.fut.wait();
    for (auto& ref : cons_arr) ref.fut.wait();

    // calc statistics
    stat_data average_prod_stat{};
    if(prod_thread_num > 0)
    {
        average_prod_stat.min_prod_nsec = 0;
        for (size_t i = 0; i < prod_thread_num; ++i)
        {
            auto& stat = prod_arr[i].stat;
            average_prod_stat.success_producer += stat.success_producer;
            average_prod_stat.fail_producer += stat.fail_producer;
            average_prod_stat.max_prod_nsec += stat.max_prod_nsec;
            average_prod_stat.min_prod_nsec += stat.min_prod_nsec;
            average_prod_stat.nsec_total += stat.nsec_total;
        }
        average_prod_stat.max_prod_nsec /= prod_thread_num;
        average_prod_stat.min_prod_nsec /= prod_thread_num;
        average_prod_stat.call_count =
            average_prod_stat.success_producer + average_prod_stat.fail_producer;
        average_prod_stat.average_prod_nsec =
            average_prod_stat.nsec_total / average_prod_stat.call_count;
    }
    //
    stat_data average_cons_stat{};
    if(cons_thread_num > 0)
    {
        average_cons_stat.min_cons_nsec = 0;
        for (size_t i = 0; i < cons_thread_num; ++i)
        {
            auto& stat = cons_arr[i].stat;
            average_cons_stat.success_consumer += stat.success_consumer;
            average_cons_stat.fail_consumer += stat.fail_consumer;
            average_cons_stat.max_cons_nsec += stat.max_cons_nsec;
            average_cons_stat.min_cons_nsec += stat.min_cons_nsec;
            average_cons_stat.nsec_total += stat.nsec_total;
        }
        average_cons_stat.max_cons_nsec /= cons_thread_num;
        average_cons_stat.min_cons_nsec /= cons_thread_num;
        average_cons_stat.call_count =
            average_cons_stat.success_consumer + average_cons_stat.fail_consumer;
        average_cons_stat.average_cons_nsec =
            average_cons_stat.nsec_total / average_cons_stat.call_count;
    }

    // print statistics
    std::cout << "producer, threads number: " << prod_thread_num << std::endl;
    std::cout << "  success_producer: "
        << average_prod_stat.success_producer << std::endl;
    std::cout << "  fail_producer: " << average_prod_stat.fail_producer << std::endl;
    std::cout << "  max_prod_nsec: " << average_prod_stat.max_prod_nsec << std::endl;
    std::cout << "  min_prod_nsec: " << average_prod_stat.min_prod_nsec << std::endl;
    std::cout << "  average_prod_nsec: "
        << average_prod_stat.average_prod_nsec << std::endl;
    std::cout << "consumer, thread number: " << cons_thread_num << std::endl;
    std::cout << "  success_consumer: "
        << average_cons_stat.success_consumer << std::endl;
    std::cout << "  fail_consumer: " << average_cons_stat.fail_consumer << std::endl;
    std::cout << "  max_cons_nsec: " << average_cons_stat.max_cons_nsec << std::endl;
    std::cout << "  min_cons_nsec: " << average_cons_stat.min_cons_nsec << std::endl;
    std::cout << "  average_cons_nsec: "
        << average_cons_stat.average_cons_nsec << std::endl;
    //std::cout << "nodes cnt: " << structure.get_nodes_count() << std::endl;

    return 0;
}














