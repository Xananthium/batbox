"""Reject failed OpenIE cache entries without discarding a graph checkpoint."""
import hashlib
import json
import logging
import os
from pathlib import Path
import sqlite3
import tempfile

LOG = logging.getLogger(__name__)


def _sync_directory(directory):
    fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def extraction_error(field, message, metadata):
    """Check the extraction contract before HippoRAG persists its result."""
    if metadata.get("finish_reason") not in {"stop", "length"}:
        return f"{field} extraction did not finish normally ({metadata.get('finish_reason')!r})"
    if metadata.get("error"):
        return f"{field} extraction reported an error"
    from hipporag.information_extraction.openie_openai import (
        _extract_json_list_field, _extract_ner_from_response,
    )
    from hipporag.utils.llm_utils import fix_broken_generated_json
    try:
        if metadata.get("finish_reason") == "length":
            message = fix_broken_generated_json(message)
        values = (_extract_ner_from_response(message) if field == "named_entities"
                  else _extract_json_list_field(message, field))
        valid = (all(isinstance(value, str) for value in values) if field == "named_entities"
                 else all(isinstance(value, list) and len(value) == 3
                          and all(isinstance(part, str) for part in value) for value in values))
        if not valid:
            return f"{field} extraction has invalid item types"
    except (TypeError, ValueError):
        return f"{field} extraction does not contain a valid JSON list"
    return None


def checked_extraction_infer(llm, infer, messages, kwargs, field):
    """Preserve repairable results; retry a failed capped extraction once."""
    from hipporag.llm.base import normalize_generation_token_params
    params = dict(kwargs)
    retry_params = normalize_generation_token_params(llm.llm_config.generate_params, params, "max_new_tokens")
    limit = retry_params.get("max_new_tokens")
    can_increase = isinstance(limit, int) and 0 < limit < 4096
    attempts = 2
    # The private quarantine receipt survives restarts. Once this exact request
    # failed at its normal cap, resume its one larger variant instead of issuing
    # the known-failing small request on every checkpoint replay.
    if can_increase and previously_failed_at_limit(llm, messages, params, field):
        retry_params["max_new_tokens"] = min(limit * 2, 4096)
        params, attempts = retry_params, 1
    for attempt in range(attempts):
        result = infer(messages=messages, **params)
        message, metadata, _cached = result
        error = extraction_error(field, message, metadata)
        if error is None:
            return result
        removed = quarantine_failed_response(llm, messages, params, message, metadata, error)
        LOG.warning("Rejected %s response; quarantined cache row: %s", field, removed)
        if (attempt == 0 and attempts == 2 and metadata.get("finish_reason") == "length" and can_increase):
            retry_params["max_new_tokens"] = min(limit * 2, 4096)
            LOG.warning("Retrying failed %s extraction with token budget %s (was %s)",
                        field, retry_params["max_new_tokens"], limit)
            params = retry_params
            continue
        raise ValueError(error)


def previously_failed_at_limit(llm, messages, kwargs, field):
    key = extraction_cache_key(llm, messages, kwargs)
    directory = Path(llm.cache_file_name).parent / "rejected-extractions"
    for path in directory.glob(f"{key}-*.json"):
        receipt = json.loads(path.read_text())
        metadata = json.loads(receipt["metadata"])
        if (receipt["key"] == key and receipt["cache"] == str(llm.cache_file_name)
                and metadata.get("finish_reason") == "length"
                and extraction_error(field, receipt["message"], metadata)):
            return True
    return False


def extraction_cache_key(llm, messages, kwargs):
    """Match the installed CacheOpenAI schema-3 key, including the subclass."""
    from hipporag.llm.base import normalize_generation_token_params
    config = llm.global_config
    data = normalize_generation_token_params(llm.llm_config.generate_params, kwargs, "_max_tokens")
    data.update(messages=messages, _cache_schema=3,
                _provider_class=f"{llm.__class__.__module__}.{llm.__class__.__qualname__}",
                _endpoint=getattr(config, "azure_endpoint", None) or getattr(config, "llm_base_url", None),
                _azure_api_version=getattr(config, "azure_api_version", None),
                _azure_chat_deployment=getattr(config, "azure_chat_deployment", None))
    return hashlib.sha256(json.dumps(data, sort_keys=True, default=str).encode()).hexdigest()


def quarantine_failed_response(llm, messages, kwargs, message, metadata, reason):
    """Back up and remove only this response, under CacheOpenAI's own locks.

    The row comparison preserves a newer response if another process replaced
    this key between inference and validation. No graph/OpenIE state is changed.
    """
    from filelock import FileLock
    if kwargs.get("store") is True or llm.llm_config.generate_params.get("store") is True:
        return False
    key = extraction_cache_key(llm, messages, kwargs)
    cache = str(llm.cache_file_name)
    with FileLock(f"{cache}.{key[:2]}.lock"), FileLock(cache + ".lock"):
        with sqlite3.connect(cache) as connection:
            row = connection.execute("SELECT message,metadata FROM cache WHERE key=?", (key,)).fetchone()
            if row is None or row[0] != message or json.loads(row[1]) != metadata:
                return False
            directory = Path(cache).parent / "rejected-extractions"
            try:
                directory.mkdir(mode=0o700)
            except FileExistsError:
                pass
            else:
                _sync_directory(directory.parent)
            os.chmod(directory, 0o700)
            digest = hashlib.sha256(json.dumps(row).encode()).hexdigest()
            backup = directory / f"{key}-{digest}.json"
            payload = json.dumps({"cache": cache, "key": key, "message": row[0],
                                  "metadata": row[1], "reason": reason}, indent=2) + "\n"
            if backup.exists():
                # A previous retry already backed up this exact response.
                if backup.read_text() != payload:
                    raise RuntimeError("Extraction quarantine backup does not match the cached response")
            else:
                # An interrupted write must not become a visible retry receipt.
                fd, temporary = tempfile.mkstemp(prefix=".quarantine-", suffix=".tmp", dir=directory)
                try:
                    with os.fdopen(fd, "w") as stream:
                        stream.write(payload)
                        stream.flush()
                        os.fsync(stream.fileno())
                    os.replace(temporary, backup)
                finally:
                    Path(temporary).unlink(missing_ok=True)
            _sync_directory(directory)
            connection.execute("DELETE FROM cache WHERE key=? AND message=? AND metadata=?", (key, *row))
    return True
