#ifndef __LOCKED_STACK_HPP__
#define __LOCKED_STACK_HPP__

#include <mutex>
#include <stack>

#include <boost/noncopyable.hpp>

#include "../technical.hpp"



namespace locked
{
	//
    template <
        typename T,
        typename Lock
    > class locked_stack: boost::noncopyable
    {
    private:
        using value_type = T;
        using lock_type = Lock;

    public:
        locked_stack() = default;

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
            value = std::move( m_data.top() );
            m_data.pop();
            return true;
        }

    private:
        std::stack<value_type> m_data;
        lock_type m_synch;
    };
	//
}

#endif // __LOCKED_STACK_HPP__
