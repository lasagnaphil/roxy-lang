#pragma once

// A very lightweight unique_ptr class that doesn't include a bunch of STL headers.

namespace rx {

template <class T>
class UniquePtr {
private:
	T* m_data;
public:
	UniquePtr() : m_data(nullptr) {}
	UniquePtr(const UniquePtr&) = delete;
	UniquePtr& operator=(const UniquePtr&) = delete;
	UniquePtr(T* data) : m_data(data) {}
	UniquePtr(UniquePtr&& other) : m_data(other.m_data) {
		other.m_data = nullptr;
	}
	~UniquePtr() { reset(); }

    UniquePtr &operator=(UniquePtr&& other) {
        reset();
        m_data = other.m_data;
        other.m_data = nullptr;
        return *this;
    }

	void reset() noexcept(true) {
		delete m_data;
		m_data = nullptr;
	}

	T* get() { return m_data; }
	const T* get() const { return m_data; }
	T* operator->() { return m_data; }
	const T* operator->() const { return m_data; }

	T& operator*() { return *m_data; }
	const T& operator*() const { return *m_data; }

	T* release() {
		T* tmp = m_data;
		m_data = nullptr;
		return tmp;
	}
};

template <typename T, typename ... Args>
UniquePtr<T> make_unique(Args&&... args) { return UniquePtr<T>(new T(args...)); }

}