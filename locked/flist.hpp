#ifndef __LOCKED_FLIST_HPP__
#define __LOCKED_FLIST_HPP__

#include <mutex>
#include <forward_list>
#include <algorithm>

#include <boost/noncopyable.hpp>

#include "../technical.hpp"



namespace locked
{
    //
    template <
        typename T,
        typename Lock
    > class flist: boost::noncopyable
    {
    private:
        using value_type = T;
        using lock_type = Lock;

    public:
        flist() = default;

        bool contains(const value_type& value)
        {
            std::lock_guard<lock_type> lck(m_synch);
            auto it = std::find(m_data.begin(), m_data.end(), value);
            return it != m_data.end() ? true : false;
        }

        bool add(const value_type& value)
        {
            std::lock_guard<lock_type> lck(m_synch);

            auto it = m_data.begin();
            auto prev = it;
            auto end = m_data.end();
            for(; it != end; ++it)
            {
                if(*it >= value) break;
                prev = it;
            }

            if(it != end && *it == value) return true;
            if(it == prev) // m_data.begin()
                m_data.push_front(value);
            else
                m_data.insert_after(prev, value);

            return true;
        }

        bool remove(const value_type& value)
        {
            std::lock_guard<lock_type> lck(m_synch);
            m_data.remove(value);
            return true;
        }

    private:
        std::forward_list<value_type> m_data;
        lock_type m_synch;
    };
    //
}

#endif // __LOCKED_FLIST_HPP__












