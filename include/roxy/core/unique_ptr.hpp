#pragma once

#include <cstddef>
#include "roxy/core/types.hpp"

// A very lightweight unique_ptr class that doesn't include a bunch of STL headers.

namespace rx {

template <class T>
class UniquePtr {
private:
	T* m_data;
public:
	RX_FORCEINLINE UniquePtr() : m_data(nullptr) {}
	UniquePtr(const UniquePtr&) = delete;
	UniquePtr& operator=(const UniquePtr&) = delete;
	RX_FORCEINLINE UniquePtr(T* data) : m_data(data) {}
	RX_FORCEINLINE UniquePtr(UniquePtr&& other) : m_data(other.m_data) {
		other.m_data = nullptr;
	}
	RX_FORCEINLINE ~UniquePtr() { reset(); }

	RX_FORCEINLINE UniquePtr& operator=(UniquePtr&& other) {
		reset();
		m_data = other.m_data;
		other.m_data = nullptr;
		return *this;
	}

	RX_FORCEINLINE void reset() noexcept(true) {
		delete m_data;
		m_data = nullptr;
	}

	RX_FORCEINLINE T* get() { return m_data; }
	RX_FORCEINLINE const T* get() const { return m_data; }
	RX_FORCEINLINE T* operator->() { return m_data; }
	RX_FORCEINLINE const T* operator->() const { return m_data; }

	RX_FORCEINLINE T& operator*() { return *m_data; }
	RX_FORCEINLINE const T& operator*() const { return *m_data; }

	RX_FORCEINLINE T* release() {
		T* tmp = m_data;
		m_data = nullptr;
		return tmp;
	}

	RX_FORCEINLINE explicit operator bool() const { return m_data != nullptr; }

	RX_FORCEINLINE bool operator==(std::nullptr_t) const { return m_data == nullptr; }
	RX_FORCEINLINE bool operator!=(std::nullptr_t) const { return m_data != nullptr; }
};

template <typename T, typename ... Args>
RX_FORCEINLINE UniquePtr<T> make_unique(Args&&... args) { return UniquePtr<T>(new T(args...)); }

// Partial specialization for arrays
template <class T>
class UniquePtr<T[]> {
private:
	T* m_data;

public:
	RX_FORCEINLINE UniquePtr() : m_data(nullptr) {}
	UniquePtr(const UniquePtr&) = delete;
	UniquePtr& operator=(const UniquePtr&) = delete;
	RX_FORCEINLINE UniquePtr(T* data) : m_data(data) {}
	RX_FORCEINLINE UniquePtr(UniquePtr&& other) : m_data(other.m_data) {
		other.m_data = nullptr;
	}
	RX_FORCEINLINE ~UniquePtr() { reset(); }

	RX_FORCEINLINE UniquePtr& operator=(UniquePtr&& other) {
		reset();
		m_data = other.m_data;
		other.m_data = nullptr;
		return *this;
	}

	RX_FORCEINLINE void reset() noexcept(true) {
		delete[] m_data;
		m_data = nullptr;
	}

	RX_FORCEINLINE T* get() { return m_data; }
	RX_FORCEINLINE const T* get() const { return m_data; }

	RX_FORCEINLINE T& operator[](size_t i) { return m_data[i]; }
	RX_FORCEINLINE const T& operator[](size_t i) const { return m_data[i]; }

	RX_FORCEINLINE T* release() {
		T* tmp = m_data;
		m_data = nullptr;
		return tmp;
	}

	RX_FORCEINLINE explicit operator bool() const { return m_data != nullptr; }

	RX_FORCEINLINE bool operator==(std::nullptr_t) const { return m_data == nullptr; }
	RX_FORCEINLINE bool operator!=(std::nullptr_t) const { return m_data != nullptr; }
};

}
