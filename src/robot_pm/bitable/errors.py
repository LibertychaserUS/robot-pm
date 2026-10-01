"""Errors for the Bitable adapter. Messages are scrubbed of known secrets."""

from __future__ import annotations

from urllib.parse import quote


def scrub(text: str, secrets: tuple[str, ...] | list[str]) -> str:
    """Replace known secret values, including their URL-encoded forms."""
    redacted = text
    for secret in secrets:
        if not secret:
            continue
        redacted = redacted.replace(secret, "[redacted]")
        encoded = quote(secret, safe="")
        if encoded and encoded != secret:
            redacted = redacted.replace(encoded, "[redacted]")
    return redacted


class BitableError(Exception):
    """Base error. The string form never contains the secrets passed in."""

    def __init__(self, message: str, *, secrets: tuple[str, ...] = ()) -> None:
        self.secrets = tuple(secret for secret in secrets if secret)
        super().__init__(scrub(message, self.secrets))


class ConfigError(BitableError):
    """Required configuration is missing or empty."""


class AuthError(BitableError):
    """tenant_access_token could not be obtained or was rejected."""


class FieldError(BitableError):
    """The edit file names an unknown field or uses an unsupported value."""


class BitableAPIError(BitableError):
    """Feishu returned a non-zero code or an unexpected body."""

    def __init__(
        self,
        message: str,
        *,
        code: int | None = None,
        http_status: int | None = None,
        secrets: tuple[str, ...] = (),
    ) -> None:
        super().__init__(message, secrets=secrets)
        self.code = code
        self.http_status = http_status


class RateLimitError(BitableAPIError):
    """Feishu asked the caller to slow down. This client does not retry."""


class PartialBatchError(BitableError):
    """A batch call applied some records and then stopped."""

    def __init__(
        self,
        message: str,
        *,
        applied: list[str],
        rejected: list[str],
        secrets: tuple[str, ...] = (),
    ) -> None:
        super().__init__(message, secrets=secrets)
        self.applied = list(applied)
        self.rejected = list(rejected)


class EditFailed(BitableError):
    """An edit file stopped after one operation failed. Earlier writes remain."""

    def __init__(
        self,
        message: str,
        *,
        completed: list[dict],
        secrets: tuple[str, ...] = (),
    ) -> None:
        super().__init__(message, secrets=secrets)
        self.completed = completed
