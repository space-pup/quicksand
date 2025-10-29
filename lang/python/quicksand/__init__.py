from .quicksand import Connection, now, delete, ns, ns_elapsed, sleep

# Alias connection object
connection = Connection

# Version
try:
    from importlib import metadata
except ImportError:
    import importlib_metadata as metadata  # Python <3.8 fallback

__version__ = metadata.version(__package__ or __name__)
