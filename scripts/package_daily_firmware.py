"""Package existing UF2 bytes; never rebuild or modify firmware."""
import argparse
import hashlib
import shutil
from pathlib import Path

FILES = {
    "cornix_prospector_nape_bridge_nosd.uf2": "Prospector",
    "cornix_prospector_nape_bridge_no_inertia_nosd.uf2": "Prospector",
    "cornix_dongle_nosd.uf2": "Cornix",
    "cornix_left_default_nosd.uf2": "Cornix",
    "cornix_left_for_dongle_nosd.uf2": "Cornix",
    "cornix_right_nosd.uf2": "Cornix",
    "reset_prospector_xiao_ble_nosd.uf2": "Reset",
    "cornix_reset.uf2": "Reset",
    "reset_nicenano_nosd.uf2": "Reset",
}


def package(source, destination, sha, run_id):
    # Fail before copying if a matrix target is missing or ambiguous.
    selected = {}
    for name in FILES:
        matches = list(source.rglob(name))
        if len(matches) != 1 or not matches[0].is_file():
            raise ValueError(f"Expected exactly one {name}; found {len(matches)}")
        selected[name] = matches[0]
    if destination.exists():
        raise ValueError("Output directory must be new to exclude stale diagnostic files")
    destination.mkdir(parents=True)
    hashes = []
    for name, original in selected.items():
        relative = Path(FILES[name]) / name
        target = destination / relative
        target.parent.mkdir(exist_ok=True)
        shutil.copyfile(original, target)
        digest = hashlib.sha256(target.read_bytes()).hexdigest()
        if digest != hashlib.sha256(original.read_bytes()).hexdigest():
            raise ValueError(f"Copy verification failed: {name}")
        hashes.append(f"{digest}  {relative.as_posix()}")
    (destination / "SHA256SUMS.txt").write_text("\n".join(hashes) + "\n", encoding="utf-8")
    readme = f"""# 日常用ファームウェア

元ビルド: https://github.com/haneta007/cornix-lp-zmk-/actions/runs/{run_id}
元コミット: {sha}
UF2は元ビルドからそのままコピーしています。

## 今回使うもの
Prospector/cornix_prospector_nape_bridge_nosd.uf2
→ Prospectorへ手動書き込み。カーソル慣性あり、Nape対応、Studio修正入り。

## 慣性なしで比較する場合
Prospector/cornix_prospector_nape_bridge_no_inertia_nosd.uf2
→ Prospectorへ手動書き込み。加速度・スクロール設定は通常版と同じ。

## Cornix
Cornix/cornix_left_for_dongle_nosd.uf2 → Prospectorと使うCornix左
Cornix/cornix_right_nosd.uf2 → Cornix右
Cornix/cornix_left_default_nosd.uf2 → 左を単体centralとして使う別構成
Cornix/cornix_dongle_nosd.uf2 → nice_nano用dongle別構成。Prospector用ではありません。
今回のProspector更新だけならCornix左右の書き込みは不要です。

## Reset
Reset/reset_prospector_xiao_ble_nosd.uf2 → Prospector用設定リセット
Reset/cornix_reset.uf2 → Cornix用設定リセット（既存cornix_rightターゲット）
Reset/reset_nicenano_nosd.uf2 → nice_nano用設定リセット
通常の更新にはリセットを使いません。これらは動作用ファームウェアではありません。
リセットすると保存済み設定・ペアリングが消えるため、必要な場合だけ使用し、その後動作用UF2を書き戻します。

ログ版・診断版・Nape bond削除専用版は含めていません。
"""
    (destination / "README.txt").write_text(readme, encoding="utf-8")
    print(f"Packaged {len(selected)} UF2 files; hashes verified")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--sha", required=True)
    parser.add_argument("--run-id", required=True)
    args = parser.parse_args()
    package(args.source, args.destination, args.sha, args.run_id)
