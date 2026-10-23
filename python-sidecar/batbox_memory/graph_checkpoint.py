"""Write-ahead checkpoints for HippoRAG's multi-file parquet index.

The pinned HippoRAG version replaces graph, vector and OpenIE files atomically,
but updates chunk metadata in place. Hard links keep the former cheap; metadata
is copied. SQLite embedding and LLM caches remain outside rollback, so work done
by an interrupted operation can be reused when its durable request is replayed.
"""
import json
import os
import shutil
from pathlib import Path


def sync_directory(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8") as stream:
        json.dump(value, stream, ensure_ascii=False)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)
    sync_directory(path.parent)


class IndexCheckpoint:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.journal = self.directory / "pending-index.json"
        self.snapshot = self.directory / ".index-checkpoint"

    def read(self):
        return json.loads(self.journal.read_text()) if self.journal.exists() else None

    @staticmethod
    def paths(rag):
        return [Path(p) for p in (
            rag._graph_pickle_filename, rag.chunk_metadata_path,
            rag.index_manifest_path, rag.openie_state_path, rag.openie_results_path,
            rag.chunk_embedding_store.filename, rag.entity_embedding_store.filename,
            rag.fact_embedding_store.filename,
        )]

    def begin(self, rag, operation):
        if self.journal.exists():
            raise RuntimeError("Pending graph operation must be recovered before starting another")
        # A kill before journal publication leaves only an unused checkpoint.
        if self.snapshot.exists():
            shutil.rmtree(self.snapshot)
        self.snapshot.mkdir(parents=True)
        files = {}
        for path in self.paths(rag):
            relative = path.relative_to(self.directory)
            files[str(relative)] = path.exists()
            if path.exists():
                target = self.snapshot / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                if path == Path(rag.chunk_metadata_path):
                    shutil.copy2(path, target)
                else:
                    os.link(path, target)
                with target.open("rb") as stream:
                    os.fsync(stream.fileno())
        for path in sorted(self.snapshot.rglob("*"), reverse=True):
            if path.is_dir():
                sync_directory(path)
        sync_directory(self.snapshot)
        value = dict(operation, version=1, files=files,
                     openie_state=str(Path(rag.openie_state_path).relative_to(self.directory)))
        atomic_json(self.journal, value)
        return value

    def restore(self, operation):
        if operation.get("version") != 1 or not self.snapshot.is_dir():
            raise RuntimeError("Graph checkpoint is missing or has an unsupported version")
        # Keep extraction results produced since the checkpoint. A second kill
        # during restore must not replace them with the older restored version.
        extracted = self.snapshot / "interrupted-openie.json"
        current = self.directory / operation["openie_state"]
        if not extracted.exists() and current.exists():
            temporary = extracted.with_suffix(".tmp")
            shutil.copy2(current, temporary)
            with temporary.open("rb") as stream:
                os.fsync(stream.fileno())
            os.replace(temporary, extracted)
            sync_directory(self.snapshot)
        for relative, existed in operation["files"].items():
            path = self.directory / relative
            if existed:
                source = self.snapshot / relative
                if not source.is_file():
                    raise RuntimeError(f"Graph checkpoint component is missing: {relative}")
                temporary = path.with_suffix(path.suffix + ".restore")
                # Copy: metadata is subsequently overwritten in place and must
                # never mutate the checkpoint used by another recovery attempt.
                shutil.copy2(source, temporary)
                with temporary.open("rb") as stream:
                    os.fsync(stream.fileno())
                os.replace(temporary, path)
            else:
                path.unlink(missing_ok=True)
            sync_directory(path.parent)

    def recovered_extractions(self, rag, docs):
        path = self.snapshot / "interrupted-openie.json"
        if not path.exists():
            return []
        try:
            value = json.loads(path.read_text())
            if value.get("provenance") != rag._current_openie_provenance():
                return []
            # Never restore removed chunks or change extraction provenance.
            wanted = set(docs)
            return [row for row in value["docs"] if row["passage"] in wanted]
        except (ValueError, KeyError, TypeError):
            # A partial cache is not authoritative; the LLM cache can still help.
            return []

    def sync_index(self, rag):
        parents = set()
        for path in self.paths(rag):
            if path.exists():
                with path.open("rb") as stream:
                    os.fsync(stream.fileno())
                parents.add(path.parent)
        for path in parents:
            sync_directory(path)

    def finish(self):
        # The durable marker goes last, after both HippoRAG and SQLite commits.
        self.journal.unlink()
        sync_directory(self.directory)
        shutil.rmtree(self.snapshot)
