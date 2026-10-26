"""Independent T1 translation jobs with a replaceable provider.

This package intentionally does not import ``companion.tasks``. It owns a
separate SQLite metadata ledger, a separate bearer capability and in-memory
text results that never enter the ledger or the task journals. The provider
boundary accepts a bounded MiMo adapter or the local synthetic implementation.
"""
