//! Memory pool for zero-allocation hot paths

use std::cell::UnsafeCell;
use std::sync::atomic::{AtomicPtr, AtomicUsize, Ordering};
use std::ptr::NonNull;
use std::marker::PhantomData;

/// A fixed-size memory pool for fast allocation
pub struct MemoryPool<T> {
    blocks: Box<[UnsafeCell<Block<T>>]>,
    free_list: AtomicPtr<Block<T>>,
    allocated: AtomicUsize,
    capacity: usize,
}

struct Block<T> {
    data: Option<T>,
    next: *mut Block<T>,
}

// Safety: MemoryPool can be safely shared between threads
unsafe impl<T: Send> Send for MemoryPool<T> {}
unsafe impl<T: Send> Sync for MemoryPool<T> {}

impl<T> MemoryPool<T> {
    /// Create a new memory pool with the given capacity
    pub fn new(capacity: usize) -> Self {
        let mut blocks: Vec<UnsafeCell<Block<T>>> = Vec::with_capacity(capacity);

        for _ in 0..capacity {
            blocks.push(UnsafeCell::new(Block {
                data: None,
                next: std::ptr::null_mut(),
            }));
        }

        let blocks = blocks.into_boxed_slice();

        // Build free list
        for i in 0..capacity - 1 {
            unsafe {
                (*blocks[i].get()).next = blocks[i + 1].get();
            }
        }

        let free_list = AtomicPtr::new(blocks[0].get());

        MemoryPool {
            blocks,
            free_list,
            allocated: AtomicUsize::new(0),
            capacity,
        }
    }

    /// Allocate an item from the pool
    pub fn allocate(&self, value: T) -> Option<PoolHandle<T>> {
        loop {
            let head = self.free_list.load(Ordering::Acquire);
            if head.is_null() {
                return None; // Pool exhausted
            }

            unsafe {
                let next = (*head).next;
                if self.free_list.compare_exchange_weak(
                    head,
                    next,
                    Ordering::Release,
                    Ordering::Relaxed,
                ).is_ok() {
                    (*head).data = Some(value);
                    (*head).next = std::ptr::null_mut();
                    self.allocated.fetch_add(1, Ordering::Relaxed);
                    return Some(PoolHandle {
                        ptr: NonNull::new_unchecked(head),
                        _marker: PhantomData,
                    });
                }
            }
        }
    }

    /// Deallocate an item back to the pool
    ///
    /// # Safety
    /// The handle must have been allocated from this pool
    pub unsafe fn deallocate(&self, handle: PoolHandle<T>) {
        let block = handle.ptr.as_ptr();

        // Clear the data
        (*block).data = None;

        // Add back to free list
        loop {
            let head = self.free_list.load(Ordering::Relaxed);
            (*block).next = head;
            if self.free_list.compare_exchange_weak(
                head,
                block,
                Ordering::Release,
                Ordering::Relaxed,
            ).is_ok() {
                break;
            }
        }

        self.allocated.fetch_sub(1, Ordering::Relaxed);
        std::mem::forget(handle); // Don't run destructor
    }

    /// Get number of allocated blocks
    pub fn allocated_count(&self) -> usize {
        self.allocated.load(Ordering::Relaxed)
    }

    /// Get number of available blocks
    pub fn available_count(&self) -> usize {
        self.capacity - self.allocated_count()
    }

    /// Get pool capacity
    pub fn capacity(&self) -> usize {
        self.capacity
    }
}

/// Handle to a pool-allocated item
pub struct PoolHandle<T> {
    ptr: NonNull<Block<T>>,
    _marker: PhantomData<T>,
}

impl<T> PoolHandle<T> {
    /// Get a reference to the allocated value
    pub fn get(&self) -> Option<&T> {
        unsafe { (*self.ptr.as_ptr()).data.as_ref() }
    }

    /// Get a mutable reference to the allocated value
    pub fn get_mut(&mut self) -> Option<&mut T> {
        unsafe { (*self.ptr.as_ptr()).data.as_mut() }
    }
}

// Safety: PoolHandle can be sent between threads if T can
unsafe impl<T: Send> Send for PoolHandle<T> {}

/// Object pool that provides smart pointers
pub struct ObjectPool<T> {
    inner: MemoryPool<T>,
}

impl<T> ObjectPool<T> {
    pub fn new(capacity: usize) -> Self {
        ObjectPool {
            inner: MemoryPool::new(capacity),
        }
    }

    pub fn acquire(&self, value: T) -> Option<PooledObject<T>> {
        self.inner.allocate(value).map(|handle| PooledObject {
            handle: Some(handle),
            pool: self as *const Self,
        })
    }

    pub fn allocated_count(&self) -> usize {
        self.inner.allocated_count()
    }

    pub fn available_count(&self) -> usize {
        self.inner.available_count()
    }
}

/// A pooled object that returns to pool on drop
pub struct PooledObject<T> {
    handle: Option<PoolHandle<T>>,
    pool: *const ObjectPool<T>,
}

impl<T> std::ops::Deref for PooledObject<T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        self.handle.as_ref().unwrap().get().unwrap()
    }
}

impl<T> std::ops::DerefMut for PooledObject<T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        self.handle.as_mut().unwrap().get_mut().unwrap()
    }
}

impl<T> Drop for PooledObject<T> {
    fn drop(&mut self) {
        if let Some(handle) = self.handle.take() {
            unsafe {
                (*self.pool).inner.deallocate(handle);
            }
        }
    }
}

// Safety: PooledObject can be sent if T can
unsafe impl<T: Send> Send for PooledObject<T> {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_memory_pool_basic() {
        let pool: MemoryPool<i32> = MemoryPool::new(4);

        assert_eq!(pool.available_count(), 4);

        let h1 = pool.allocate(1).unwrap();
        let h2 = pool.allocate(2).unwrap();

        assert_eq!(pool.allocated_count(), 2);
        assert_eq!(*h1.get().unwrap(), 1);
        assert_eq!(*h2.get().unwrap(), 2);

        unsafe {
            pool.deallocate(h1);
        }
        assert_eq!(pool.allocated_count(), 1);
    }

    #[test]
    fn test_object_pool() {
        let pool: ObjectPool<String> = ObjectPool::new(4);

        {
            let obj = pool.acquire("hello".to_string()).unwrap();
            assert_eq!(&*obj, "hello");
            assert_eq!(pool.allocated_count(), 1);
        }

        // Should be returned to pool
        assert_eq!(pool.allocated_count(), 0);
    }
}
