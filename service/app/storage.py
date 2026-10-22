"""Where finished runs live once the process that made them is gone.

Cloud Run scales to zero and its disk goes with the instance, so a run written
to /tmp is copied to a bucket and read back from there. Locally there is no
bucket and the run directory is the store.

Conversations with the assistant are kept the same way, one JSON document each:
under _conversations/ beside the runs locally, under conversations/ in the
bucket.
"""

from __future__ import annotations

import json
from pathlib import Path

RUN_FILES = ("form.json", "results.json", "curves.json", "equity.csv", "status.json")


class LocalStore:
    def __init__(self, root: Path):
        self.root = root

    def save(self, job_id: str, directory: Path) -> None:
        pass  # the run directory already is the store

    def read(self, job_id: str, name: str) -> bytes | None:
        path = self.root / job_id / name
        return path.read_bytes() if path.exists() else None

    def list_ids(self) -> list[str]:
        if not self.root.exists():
            return []
        return [p.name for p in self.root.iterdir() if (p / "status.json").exists()]

    def put_conversation(self, conversation_id: str, data: bytes) -> None:
        path = self.root / "_conversations" / f"{conversation_id}.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def get_conversation(self, conversation_id: str) -> bytes | None:
        path = self.root / "_conversations" / f"{conversation_id}.json"
        return path.read_bytes() if path.exists() else None

    def conversation_ids(self) -> list[str]:
        directory = self.root / "_conversations"
        return [p.stem for p in directory.glob("*.json")] if directory.exists() else []


class GcsStore:
    """Runs under gs://<bucket>/runs/<job_id>/."""

    def __init__(self, bucket: str, local: LocalStore):
        from google.cloud import storage  # only needed when a bucket is configured

        self.bucket = storage.Client().bucket(bucket)
        self.local = local

    def save(self, job_id: str, directory: Path) -> None:
        for name in RUN_FILES:
            path = directory / name
            if path.exists():
                self.bucket.blob(f"runs/{job_id}/{name}").upload_from_filename(str(path))

    def read(self, job_id: str, name: str) -> bytes | None:
        cached = self.local.read(job_id, name)
        if cached is not None:
            return cached
        blob = self.bucket.blob(f"runs/{job_id}/{name}")
        return blob.download_as_bytes() if blob.exists() else None

    def list_ids(self) -> list[str]:
        ids = set(self.local.list_ids())
        for blob in self.bucket.list_blobs(prefix="runs/"):
            parts = blob.name.split("/")
            if len(parts) == 3 and parts[2] == "status.json":
                ids.add(parts[1])
        return sorted(ids)

    def put_conversation(self, conversation_id: str, data: bytes) -> None:
        self.local.put_conversation(conversation_id, data)
        self.bucket.blob(f"conversations/{conversation_id}.json").upload_from_string(
            data, content_type="application/json")

    def get_conversation(self, conversation_id: str) -> bytes | None:
        cached = self.local.get_conversation(conversation_id)
        if cached is not None:
            return cached
        blob = self.bucket.blob(f"conversations/{conversation_id}.json")
        return blob.download_as_bytes() if blob.exists() else None

    def conversation_ids(self) -> list[str]:
        ids = set(self.local.conversation_ids())
        for blob in self.bucket.list_blobs(prefix="conversations/"):
            if blob.name.endswith(".json"):
                ids.add(blob.name[len("conversations/"):-len(".json")])
        return sorted(ids)


def read_json(store, job_id: str, name: str):
    raw = store.read(job_id, name)
    return None if raw is None else json.loads(raw)
