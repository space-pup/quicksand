# quicksand/quicksand.py
# ----------------------
# Public, user‑friendly wrapper around the low‑level Quicksand C module.
# --------------------------------------------------------------------------

import os
from . import _quicksand as _c


class QuicksandError(RuntimeError):
    pass


class Connection:
    """
    High‑level Python class for the quicksand API.
    """

    def __init__(self, topic: str,
                 message_size: int = -1,
                 message_rate: int = -1):
        """
        Create a connection (calls quicksand_connect).

        :param topic: name of the shared memory segment.
        :param message_size: maximum bytes per message, -1 = read from existing.
        :param message_rate: max msgs/sec, -1 = read from existing.
        """
        self.topic = topic
        self.rate = message_rate
        self._capsule, self.length, self.size = _c.connect(
            topic,
            message_size=message_size,
            message_rate=message_rate,
        )

    def close(self) -> None:
        """Close the connection (calls quicksand_disconnect)."""
        if self._capsule is not None:
            _c.disconnect(self._capsule)
            self._capsule = None

    # -----------------------------------------------------------------
    # Messaging primitives
    # -----------------------------------------------------------------
    def write(self, data: bytes | bytearray | memoryview) -> None:
        """Write a complete message to the ring buffer."""
        if self._capsule is None:
            raise QuicksandError("connection is closed")
        _c.write(self._capsule, data)

    def read(self) -> bytes | None:
        """
        Read the next message.  Returns ``None`` if nothing is available.
        A fresh ``bytes`` object sized exactly to the payload is returned.
        """
        if self._capsule is None:
            raise QuicksandError("connection is closed")

        # max_size = self.size if self.max_size is None else max_size

        # Allocate a mutable buffer that the C code can fill.
        buf = bytearray(self.size)
        return _c.read(self._capsule, buf)

    def read_latest(self) -> bytes | None:
        """
        Read the latest new message.  Returns ``None`` if no new messages.
        A fresh ``bytes`` object sized exactly to the payload is returned.
        """
        if self._capsule is None:
            raise QuicksandError("connection is closed")

        # max_size = self.size if max_size is None else max_size

        # Allocate a mutable buffer that the C code can fill.
        buf = bytearray(self.size)
        return _c.read_latest(self._capsule, buf)

    def remaining(self):
        if self._capsule is None:
            raise QuicksandError("connection is closed")
        return _c.remaining(self._capsule)

    # Support delete operation
    def __del__(self):
        self.close()

    # Support being a generator
    def __len__(self):
        return self.remaining()

    def __iter__(self):
        for i in range(len(self)):
            msg = self.read()
            if msg is None:  # No more messages
                return
            yield msg

    # -----------------------------------------------------------------
    # Context manager support
    # -----------------------------------------------------------------
    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()


def delete(topic: str) -> None:
    _c.delete(topic)


def now() -> int:
    """Raw monotonic timestamp (raw TSC on x86‑64)."""
    return _c.now()


def ns(stop: int, start: int) -> float:
    """Return nanoseconds between two time counter stamps."""
    return _c.ns(stop, start)


def ns_elapsed(start: int) -> float:
    """Return nanoseconds elapsed since the provided counter stamp"""
    return _c.ns_elapsed(start)


def sleep(ns: float) -> None:
    """Busy‑wait or sleep for the requested nanoseconds."""
    _c.sleep(ns)
