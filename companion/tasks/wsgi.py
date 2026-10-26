"""Production WSGI factory. Configuration contains paths, never inline secrets."""

import os
import atexit

from .__main__ import read_config
from .caldav import Transport
from .common import stable_id
from .service import API
from .store import Store
from .completion import CompletionWorker
from .caldav import CalDAV
from .common import TaskError


def create_app():
    try:
        config = read_config(os.environ["SUITE_TASKS_CONFIG"])
        # Constructing transport validates the origin, without a network call.
        transport = Transport(config["base_url"], config["username"], config["app_password"])
        account = stable_id(transport.dav_url, config["username"])
        store = Store(os.environ["SUITE_TASKS_DATABASE"], account)
        from .speech import MiMo
        from .creation import CreationWorker
        speech_path=os.environ.get("SUITE_TASKS_SPEECH_CONFIG")
        try:
            speech=MiMo.from_file(speech_path) if speech_path else None
        except (OSError,ValueError,TaskError):
            speech=None  # An optional voice config cannot disable existing Tasks.
        api=API(store, config["device_token"], config["admin_token"],config.get("completion_token"),speech)
        if config.get("completion_token"):
            worker=CompletionWorker(store,CalDAV(transport,account,config["display_zone"]))
            worker.creation_worker=CreationWorker(api.creations,worker.client)
            worker.start(); atexit.register(worker.stop)
            api.completion_worker=worker
        return api
    except Exception:
        raise RuntimeError("Private task-service configuration or storage is unavailable") from None
