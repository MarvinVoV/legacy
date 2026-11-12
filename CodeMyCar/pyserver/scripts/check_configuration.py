"""Validate profile/provider configuration and show memory PATH, never secrets/content."""

import argparse
import json
from pathlib import Path

from app.agent.persona import load_profile
from app.agent.preferences import PreferenceStore
from app.config import get_settings
from app.providers.config import load_provider_config


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device-id", default="DEV_1")
    parser.add_argument("--user-id", default="household")
    parser.add_argument(
        "--init-memory", action="store_true", help="Create or migrate this scope if missing"
    )
    args = parser.parse_args()
    settings = get_settings()
    config = load_provider_config(settings)
    profile = load_profile(Path(settings.agent_profile_dir))
    store = PreferenceStore(
        Path(settings.agent_data_dir) / "memories",
        template=profile.memory_template,
        max_entries=settings.agent_memory_max_entries,
        legacy_db=Path(settings.agent_data_dir) / "preferences.sqlite",
    )
    scope = store.scope(args.device_id, args.user_id)
    if args.init_memory:
        store.read_document(scope)
    print(
        json.dumps(
            {
                "providers_file": str(Path(settings.providers_config).resolve()),
                "selected": {
                    kind: {
                        "provider": getattr(config, kind).provider,
                        "credentials_available": config.available(kind),
                    }
                    for kind in ["llm", "asr", "tts"]
                },
                "profile_dir": str(Path(settings.agent_profile_dir).resolve()),
                "profile_revision": profile.fingerprint[:12],
                "memory_file": str(store.document_path(scope).resolve()),
                "memory_exists": store.document_path(scope).exists(),
                "voice_motion_enabled": settings.voice_execute_commands,
            },
            ensure_ascii=False,
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
