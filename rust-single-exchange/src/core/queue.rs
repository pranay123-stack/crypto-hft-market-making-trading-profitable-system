//! Lock-free queue implementations for HFT

use std::cell::UnsafeCell;
use std::sync::atomic::{AtomicUsize, Ordering};

/// Single-Producer Single-Consumer lock-free queue
pub struct SpscQueue<T> {
    buffer: Box<[UnsafeCell<Option<T>>]>,
    capacity: usize,
    mask: usize,
    head: AtomicUsize,
    tail: AtomicUsize,
}

// Safety: SpscQueue is safe to send between threads as long as
// only one producer and one consumer access it
unsafe impl<T: Send> Send for SpscQueue<T> {}
unsafe impl<T: Send> Sync for SpscQueue<T> {}

impl<T> SpscQueue<T> {
    /// Create a new SPSC queue with the given capacity (must be power of 2)
    pub fn new(capacity: usize) -> Self {
        assert!(capacity.is_power_of_two(), "Capacity must be power of 2");

        let buffer: Vec<UnsafeCell<Option<T>>> = (0..capacity)
            .map(|_| UnsafeCell::new(None))
            .collect();

        SpscQueue {
            buffer: buffer.into_boxed_slice(),
            capacity,
            mask: capacity - 1,
            head: AtomicUsize::new(0),
            tail: AtomicUsize::new(0),
        }
    }

    /// Try to push an item (producer only)
    pub fn try_push(&self, item: T) -> Result<(), T> {
        let tail = self.tail.load(Ordering::Relaxed);
        let next_tail = (tail + 1) & self.mask;

        if next_tail == self.head.load(Ordering::Acquire) {
            return Err(item); // Queue is full
        }

        // Safety: Only producer writes to tail position
        unsafe {
            *self.buffer[tail].get() = Some(item);
        }

        self.tail.store(next_tail, Ordering::Release);
        Ok(())
    }

    /// Try to pop an item (consumer only)
    pub fn try_pop(&self) -> Option<T> {
        let head = self.head.load(Ordering::Relaxed);

        if head == self.tail.load(Ordering::Acquire) {
            return None; // Queue is empty
        }

        // Safety: Only consumer reads from head position
        let item = unsafe { (*self.buffer[head].get()).take() };

        let next_head = (head + 1) & self.mask;
        self.head.store(next_head, Ordering::Release);

        item
    }

    /// Check if queue is empty
    pub fn is_empty(&self) -> bool {
        self.head.load(Ordering::Relaxed) == self.tail.load(Ordering::Relaxed)
    }

    /// Get approximate size
    pub fn len(&self) -> usize {
        let head = self.head.load(Ordering::Relaxed);
        let tail = self.tail.load(Ordering::Relaxed);
        (tail.wrapping_sub(head)) & self.mask
    }

    /// Get capacity
    pub fn capacity(&self) -> usize {
        self.capacity
    }
}

/// Ring buffer for sequential allocation
pub struct RingBuffer<T: Copy + Default> {
    buffer: Box<[T]>,
    capacity: usize,
    mask: usize,
    write_pos: AtomicUsize,
    read_pos: AtomicUsize,
}

unsafe impl<T: Copy + Default + Send> Send for RingBuffer<T> {}
unsafe impl<T: Copy + Default + Send> Sync for RingBuffer<T> {}

impl<T: Copy + Default> RingBuffer<T> {
    pub fn new(capacity: usize) -> Self {
        assert!(capacity.is_power_of_two());

        let buffer: Vec<T> = vec![T::default(); capacity];

        RingBuffer {
            buffer: buffer.into_boxed_slice(),
            capacity,
            mask: capacity - 1,
            write_pos: AtomicUsize::new(0),
            read_pos: AtomicUsize::new(0),
        }
    }

    pub fn write(&self, item: T) -> Option<usize> {
        let pos = self.write_pos.fetch_add(1, Ordering::Relaxed) & self.mask;

        // Check for overflow (write catching up to read)
        let read = self.read_pos.load(Ordering::Acquire);
        if pos == read && self.write_pos.load(Ordering::Relaxed) != read {
            return None;
        }

        // Safety: We have exclusive write access to this position
        unsafe {
            let ptr = self.buffer.as_ptr().add(pos) as *mut T;
            std::ptr::write(ptr, item);
        }

        Some(pos)
    }

    pub fn read(&self, pos: usize) -> Option<T> {
        if pos & self.mask >= self.capacity {
            return None;
        }
        Some(self.buffer[pos & self.mask])
    }

    pub fn advance_read(&self, pos: usize) {
        self.read_pos.store(pos, Ordering::Release);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_spsc_queue_basic() {
        let queue = SpscQueue::new(4);

        assert!(queue.try_push(1).is_ok());
        assert!(queue.try_push(2).is_ok());
        assert!(queue.try_push(3).is_ok());

        assert_eq!(queue.try_pop(), Some(1));
        assert_eq!(queue.try_pop(), Some(2));
        assert_eq!(queue.try_pop(), Some(3));
        assert_eq!(queue.try_pop(), None);
    }

    #[test]
    fn test_spsc_queue_full() {
        let queue = SpscQueue::new(2);

        assert!(queue.try_push(1).is_ok());
        assert!(queue.try_push(2).is_err()); // Full (one slot always empty)
    }
}
