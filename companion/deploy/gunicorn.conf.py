"""Loopback-only cached API behind an independently configured TLS proxy."""

bind = "127.0.0.1:8787"
workers = 1
worker_class = "gthread"
threads = 2
timeout = 30
graceful_timeout = 10
keepalive = 2
limit_request_line = 1024
limit_request_fields = 20
limit_request_field_size = 4096
forwarded_allow_ips = "127.0.0.1"
accesslog = None
errorlog = "-"
loglevel = "warning"
capture_output = False
max_requests = 2000
max_requests_jitter = 100
