#!/usr/bin/env python3
"""Universal ACMOJ client for frozen prompt-sensitivity runs."""

from __future__ import annotations

import argparse
import json
import os
from datetime import datetime
from pathlib import Path
from typing import Any

import requests


class ACMOJClient:
    def __init__(self, access_token: str) -> None:
        self.api_base = os.environ.get(
            "OJ_API_BASE",
            "https://acm.sjtu.edu.cn/OnlineJudge/api/v1",
        )
        self.headers = {
            "Authorization": f"Bearer {access_token}",
            "Content-Type": "application/x-www-form-urlencoded",
            "User-Agent": "ProjDevBench-Prompt-Sensitivity/1.0",
        }
        self.submission_log_file = os.environ.get(
            "SUBMISSION_LOG_FILE",
            "/workspace/logs/submission_ids.log",
        )

    def _request(
        self,
        method: str,
        endpoint: str,
        data: dict[str, Any] | None = None,
    ) -> dict[str, Any] | None:
        try:
            response = requests.request(
                method,
                self.api_base + endpoint,
                headers=self.headers,
                data=data,
                timeout=20,
            )
            if response.status_code == 204:
                return {"status": "success"}
            response.raise_for_status()
            return response.json() if response.content else {"status": "success"}
        except requests.RequestException as error:
            print(f"API request failed: {error}")
            response = getattr(error, "response", None)
            if response is not None:
                print(response.text[:1000])
            return None

    def _save_submission_id(self, submission_id: int) -> None:
        path = Path(self.submission_log_file)
        path.parent.mkdir(parents=True, exist_ok=True)
        record = {
            "timestamp": datetime.now().isoformat(timespec="seconds"),
            "submission_id": int(submission_id),
        }
        with path.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(record) + "\n")
        print(f"Submission ID {submission_id} saved to {path}")

    def submit_git(self, problem_id: int, git_url: str) -> dict[str, Any] | None:
        result = self._request(
            "POST",
            f"/problem/{problem_id}/submit",
            {"language": "git", "code": git_url},
        )
        if result and "id" in result:
            self._save_submission_id(int(result["id"]))
        return result

    def submit_code(
        self,
        problem_id: int,
        language: str,
        code: str,
    ) -> dict[str, Any] | None:
        result = self._request(
            "POST",
            f"/problem/{problem_id}/submit",
            {"language": language, "code": code},
        )
        if result and "id" in result:
            self._save_submission_id(int(result["id"]))
        return result

    def get_submission_detail(self, submission_id: int) -> dict[str, Any] | None:
        return self._request("GET", f"/submission/{submission_id}")

    def abort_submission(self, submission_id: int) -> dict[str, Any] | None:
        return self._request("POST", f"/submission/{submission_id}/abort")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--token", default=os.environ.get("ACMOJ_TOKEN"))
    subparsers = parser.add_subparsers(dest="command", required=True)

    submit = subparsers.add_parser("submit")
    submit.add_argument("--problem-id", type=int, required=True)
    mode = submit.add_mutually_exclusive_group(required=True)
    mode.add_argument("--git-url")
    mode.add_argument("--code-file", type=Path)
    submit.add_argument("--language")

    status = subparsers.add_parser("status")
    status.add_argument("--submission-id", type=int, required=True)

    abort = subparsers.add_parser("abort")
    abort.add_argument("--submission-id", type=int, required=True)

    args = parser.parse_args()
    if not args.token:
        raise SystemExit("ACMOJ_TOKEN is not configured")
    client = ACMOJClient(args.token)
    if args.command == "submit":
        if args.git_url:
            result = client.submit_git(args.problem_id, args.git_url)
        else:
            if not args.language:
                raise SystemExit("--language is required with --code-file")
            result = client.submit_code(
                args.problem_id,
                args.language,
                args.code_file.read_text(),
            )
    elif args.command == "status":
        result = client.get_submission_detail(args.submission_id)
    else:
        result = client.abort_submission(args.submission_id)
    if result is None:
        raise SystemExit(1)
    print(json.dumps(result, ensure_ascii=False))


if __name__ == "__main__":
    main()
