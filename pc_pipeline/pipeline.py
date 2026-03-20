from __future__ import annotations

import argparse
import csv
import importlib
import json
import math
import random
import shlex
import subprocess
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path
from typing import Any


BOARD_NAME = "Freenove ESP32-S3 WROOM CAM"
CLASS_NAMES = ["non_prone", "prone"]
SPLIT_NAMES = ("train", "val", "test")
REQUIRED_COLUMNS = [
    "capture_id",
    "timestamp_ms",
    "subject_id",
    "session_id",
    "location_id",
    "lighting_id",
    "camera_position_id",
    "annotator_id",
    "label",
    "label_name",
    "is_usable_for_training",
    "exclude_reason",
    "notes",
    "image_path",
    "image_bytes",
    "frame_width",
    "frame_height",
    "pixel_format",
    "jpeg_quality",
    "board_name",
]


@dataclass
class PipelineConfig:
    dataset_root: str
    metadata_path: str
    images_dir: str
    output_root: str
    run_name: str
    seed: int
    image_size: int
    train_ratio: float
    val_ratio: float
    test_ratio: float
    epochs: int
    batch_size: int
    learning_rate: float
    initial_threshold: float
    device: str
    espdl_converter_command: str | None


@dataclass
class RuntimeModules:
    Image: Any
    torch: Any
    nn: Any
    DataLoader: Any
    Dataset: Any


class PipelineError(Exception):
    pass


MetadataRow = dict[str, str]
JsonDict = dict[str, Any]
LabelCounts = dict[str, int]
SubjectCounts = dict[str, int]


def is_training_eligible_row(row: dict[str, str]) -> bool:
    # Basic checks for usability flags and board type.
    if not (
        row.get("is_usable_for_training") == "1"
        and row.get("exclude_reason", "") == ""
        and row.get("board_name") == BOARD_NAME
    ):
        return False

    # Validate label and label_name so that downstream aggregation (e.g. summarize_training_rows)
    # does not see unknown/invalid labels that could cause KeyError.
    label = row.get("label")
    label_name = row.get("label_name")

    if not label or not label_name:
        return False

    try:
        label_idx = int(label)
    except (TypeError, ValueError):
        return False

    if not (0 <= label_idx < len(CLASS_NAMES)):
        return False

    if CLASS_NAMES[label_idx] != label_name:
        return False

    return True
def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Mac 上で収集済みデータセットからうつ伏せ検知モデルを生成します。"
    )
    parser.add_argument("--dataset-root", required=True,
                        help="metadata.csv と images を含むディレクトリ")
    parser.add_argument("--metadata-path", help="metadata.csv のパス")
    parser.add_argument("--images-dir", help="images ディレクトリのパス")
    parser.add_argument(
        "--output-root", default="artifacts/pc_pipeline", help="成果物の出力先")
    parser.add_argument("--run-name", help="実行名")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--image-size", type=int, default=96)
    parser.add_argument("--train-ratio", type=float, default=0.70)
    parser.add_argument("--val-ratio", type=float, default=0.15)
    parser.add_argument("--test-ratio", type=float, default=0.15)
    parser.add_argument("--epochs", type=int, default=20)
    parser.add_argument("--batch-size", type=int, default=32)
    parser.add_argument("--learning-rate", type=float, default=0.001)
    parser.add_argument("--initial-threshold", type=float, default=0.50)
    parser.add_argument(
        "--device",
        default="auto",
        choices=["auto", "cpu", "mps"],
        help="Mac では auto で MPS を優先します。",
    )
    parser.add_argument(
        "--espdl-converter-command",
        help="{onnx_path} と {espdl_path} を含む変換コマンド文字列",
    )
    return parser.parse_args()


def load_runtime_modules() -> RuntimeModules:
    missing: list[str] = []
    loaded: dict[str, Any] = {}
    module_names = (
        "PIL.Image",
        "torch",
        "torch.nn",
        "torch.utils.data",
        "onnx",
    )
    for module_name in module_names:
        try:
            loaded[module_name] = importlib.import_module(module_name)
        except ModuleNotFoundError:
            missing.append(module_name.split(".")[0])

    if missing:
        unique_missing = sorted(set(missing))
        joined = ", ".join(unique_missing)
        raise PipelineError(f"依存ライブラリが不足しています: {joined}")

    data_module = loaded["torch.utils.data"]
    return RuntimeModules(
        Image=loaded["PIL.Image"],
        torch=loaded["torch"],
        nn=loaded["torch.nn"],
        DataLoader=data_module.DataLoader,
        Dataset=data_module.Dataset,
    )


def build_config(args: argparse.Namespace) -> PipelineConfig:
    dataset_root = Path(args.dataset_root).expanduser().resolve()
    metadata_path = Path(args.metadata_path).expanduser().resolve(
    ) if args.metadata_path else dataset_root / "metadata.csv"
    images_dir = Path(args.images_dir).expanduser().resolve(
    ) if args.images_dir else dataset_root / "images"
    output_root = Path(args.output_root).expanduser().resolve()
    run_name = args.run_name or datetime.now().strftime("%Y%m%d_%H%M%S")

    if args.image_size <= 0:
        raise PipelineError("image_size は 1 以上である必要があります")
    if args.epochs <= 0:
        raise PipelineError("epochs は 1 以上である必要があります")
    if args.batch_size <= 0:
        raise PipelineError("batch_size は 1 以上である必要があります")
    if args.learning_rate <= 0:
        raise PipelineError("learning_rate は 0 より大きい必要があります")
    if not 0 < args.initial_threshold < 1:
        raise PipelineError("initial_threshold は 0 と 1 の間である必要があります")

    ratio_sum = args.train_ratio + args.val_ratio + args.test_ratio
    if ratio_sum <= 0:
        raise PipelineError("分割比率の合計は正である必要があります")

    return PipelineConfig(
        dataset_root=str(dataset_root),
        metadata_path=str(metadata_path),
        images_dir=str(images_dir),
        output_root=str(output_root),
        run_name=run_name,
        seed=args.seed,
        image_size=args.image_size,
        train_ratio=args.train_ratio / ratio_sum,
        val_ratio=args.val_ratio / ratio_sum,
        test_ratio=args.test_ratio / ratio_sum,
        epochs=args.epochs,
        batch_size=args.batch_size,
        learning_rate=args.learning_rate,
        initial_threshold=args.initial_threshold,
        device=args.device,
        espdl_converter_command=args.espdl_converter_command,
    )


def read_metadata(metadata_path: Path) -> list[dict[str, str]]:
    if not metadata_path.exists():
        raise PipelineError(f"metadata.csv が見つかりません: {metadata_path}")

    with metadata_path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        fieldnames = reader.fieldnames or []
        missing_columns = [
            column for column in REQUIRED_COLUMNS
            if column not in fieldnames
        ]
        if missing_columns:
            joined = ", ".join(missing_columns)
            raise PipelineError(f"metadata.csv の必須列が不足しています: {joined}")
        return list(reader)


def collect_row_errors(
    row: MetadataRow,
    dataset_root: Path,
    runtime: RuntimeModules,
) -> list[str]:
    row_errors: list[str] = []

    for column in REQUIRED_COLUMNS:
        if row.get(column, "") == "":
            row_errors.append(f"{column} が空です")

    if row.get("board_name") != BOARD_NAME:
        row_errors.append("board_name が想定値ではありません")
    if row.get("label") not in {"0", "1"}:
        row_errors.append("label が 0 または 1 ではありません")
    if row.get("label_name") not in CLASS_NAMES:
        row_errors.append("label_name が想定値ではありません")
    if row.get("is_usable_for_training") not in {"0", "1"}:
        row_errors.append("is_usable_for_training が 0 または 1 ではありません")
    if (
        row.get("is_usable_for_training") == "1"
        and row.get("exclude_reason", "") != ""
    ):
        row_errors.append("学習利用可なのに exclude_reason が空ではありません")
    if (
        row.get("is_usable_for_training") == "0"
        and row.get("exclude_reason", "") == ""
    ):
        row_errors.append("学習利用不可なのに exclude_reason が空です")

    image_path = dataset_root / row.get("image_path", "")
    if not image_path.exists():
        row_errors.append("対応画像が存在しません")
    else:
        try:
            with runtime.Image.open(image_path) as image:
                image.verify()
        except Exception:
            row_errors.append("画像が破損しています")

    return row_errors


def summarize_training_rows(
    rows: list[MetadataRow],
) -> tuple[LabelCounts, SubjectCounts, list[JsonDict]]:
    label_counts: LabelCounts = {"0": 0, "1": 0}
    subject_counts: SubjectCounts = {}
    warnings: list[JsonDict] = []

    for row in rows:
        label_counts[row["label"]] += 1
        subject_id = row["subject_id"]
        subject_counts[subject_id] = subject_counts.get(subject_id, 0) + 1

    total_rows = len(rows)
    for subject_id, count in sorted(subject_counts.items()):
        if total_rows > 0 and count / total_rows > 0.20:
            warnings.append(
                {
                    "subject_id": subject_id,
                    "warning": "1 被写体が学習利用可サンプル全体の 20% を超えています",
                    "count": count,
                }
            )

    return label_counts, subject_counts, warnings


def audit_dataset(
    rows: list[MetadataRow],
    dataset_root: Path,
    runtime: RuntimeModules,
) -> JsonDict:
    errors: list[JsonDict] = []

    for row in rows:
        row_errors = collect_row_errors(row, dataset_root, runtime)
        if row_errors:
            errors.append({"capture_id": row.get(
                "capture_id", ""), "errors": row_errors})

    usable_rows = [row for row in rows if is_training_eligible_row(row)]
    label_counts, subject_counts, warnings = summarize_training_rows(
        usable_rows)
    return {
        "total_rows": len(rows),
        "usable_rows": len(usable_rows),
        "subject_count": len(subject_counts),
        "label_counts": label_counts,
        "errors": errors,
        "warnings": warnings,
    }


def select_training_rows(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    training_rows = [row for row in rows if is_training_eligible_row(row)]
    if not training_rows:
        raise PipelineError("学習対象行が 0 件です")
    return training_rows


def allocate_subject_counts(
    subject_count: int,
    ratios: tuple[float, float, float],
) -> tuple[int, int, int]:
    if subject_count < 3:
        raise PipelineError("学習対象 subject_id が 3 件未満のため分割できません")

    base_counts = [1, 1, 1]
    remaining = subject_count - 3
    weighted = [remaining * ratio for ratio in ratios]
    additions = [math.floor(value) for value in weighted]
    assigned = sum(additions)
    remainders = sorted(
        ((weighted[index] - additions[index], index) for index in range(3)),
        reverse=True,
    )
    for _, index in remainders[: remaining - assigned]:
        additions[index] += 1

    counts = tuple(base_counts[index] + additions[index] for index in range(3))
    if counts[1] <= 0 or counts[2] <= 0:
        raise PipelineError("val または test に subject_id を割り当てられません")
    return counts


def split_rows_by_subject(
    rows: list[MetadataRow],
    config: PipelineConfig,
) -> dict[str, list[MetadataRow]]:
    subject_ids = sorted({row["subject_id"] for row in rows})
    train_subjects, val_subjects, test_subjects = allocate_subject_counts(
        len(subject_ids),
        (config.train_ratio, config.val_ratio, config.test_ratio),
    )
    rng = random.Random(config.seed)
    rng.shuffle(subject_ids)

    train_set = set(subject_ids[:train_subjects])
    val_set = set(subject_ids[train_subjects: train_subjects + val_subjects])
    test_set = set(
        subject_ids[
            train_subjects + val_subjects:
            train_subjects + val_subjects + test_subjects
        ]
    )

    splits = {split_name: [] for split_name in SPLIT_NAMES}
    for row in rows:
        subject_id = row["subject_id"]
        if subject_id in train_set:
            splits["train"].append(row)
        elif subject_id in val_set:
            splits["val"].append(row)
        elif subject_id in test_set:
            splits["test"].append(row)

    for split_name in SPLIT_NAMES:
        if not splits[split_name]:
            raise PipelineError(f"{split_name} が空になったため学習を中止します")
    return splits


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, ensure_ascii=False,
                    indent=2), encoding="utf-8")


def write_split_csv(path: Path, rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(
            handle, fieldnames=REQUIRED_COLUMNS + ["split"])
        writer.writeheader()
        for row in rows:
            record = dict(row)
            record["split"] = path.stem
            writer.writerow(record)


def choose_device(runtime: RuntimeModules, requested: str) -> str:
    torch = runtime.torch
    mps_backend = getattr(torch.backends, "mps", None)
    mps_available = bool(
        mps_backend and torch.backends.mps.is_available()
    )
    if requested == "cpu":
        return "cpu"
    if requested == "mps":
        if mps_available:
            return "mps"
        raise PipelineError("MPS が利用できません")
    if mps_available:
        return "mps"
    return "cpu"


def build_dataset_class(runtime: RuntimeModules):
    torch = runtime.torch
    image_module = runtime.Image
    dataset_base = runtime.Dataset

    class CaptureDataset(dataset_base):
        def __init__(
            self,
            rows: list[MetadataRow],
            image_size: int,
            dataset_root: Path,
        ) -> None:
            self.rows = rows
            self.image_size = image_size
            self.dataset_root = dataset_root

        def __len__(self) -> int:
            return len(self.rows)

        def __getitem__(self, index: int) -> tuple[Any, Any]:
            row = self.rows[index]
            image_path = self.dataset_root / row["image_path"]
            with image_module.open(image_path) as image:
                resized = image.convert("RGB").resize(
                    (self.image_size, self.image_size))
                pixels = list(resized.getdata())
            tensor = torch.tensor(pixels, dtype=torch.float32).view(
                self.image_size, self.image_size, 3)
            tensor = tensor.permute(2, 0, 1) / 255.0
            label = torch.tensor(int(row["label"]), dtype=torch.long)
            return tensor, label

    return CaptureDataset


def build_data_loader(
    data_loader: Any,
    dataset: Any,
    batch_size: int,
    *,
    shuffle: bool,
) -> Any:
    return data_loader(
        dataset,
        batch_size=batch_size,
        shuffle=shuffle,
        num_workers=0,
    )


def build_model(runtime: RuntimeModules) -> Any:
    nn = runtime.nn

    return nn.Sequential(
        nn.Conv2d(3, 16, kernel_size=3, padding=1),
        nn.ReLU(),
        nn.MaxPool2d(kernel_size=2),
        nn.Conv2d(16, 32, kernel_size=3, padding=1),
        nn.ReLU(),
        nn.MaxPool2d(kernel_size=2),
        nn.Conv2d(32, 64, kernel_size=3, padding=1),
        nn.ReLU(),
        nn.AdaptiveAvgPool2d((1, 1)),
        nn.Flatten(),
        nn.Linear(64, 2),
    )


def collect_predictions(
    model: Any,
    loader: Any,
    runtime: RuntimeModules,
    device: str,
    criterion: Any,
) -> JsonDict:
    torch = runtime.torch
    model.eval()
    total_loss = 0.0
    probabilities: list[float] = []
    labels: list[int] = []

    with torch.no_grad():
        for images, targets in loader:
            images = images.to(device)
            targets = targets.to(device)
            logits = model(images)
            loss = criterion(logits, targets)
            total_loss += float(loss.item()) * int(targets.size(0))
            probs = torch.softmax(logits, dim=1)[:, 1].detach().cpu().tolist()
            probabilities.extend(float(value) for value in probs)
            labels.extend(int(value)
                          for value in targets.detach().cpu().tolist())

    count = len(labels)
    average_loss = total_loss / count if count else 0.0
    return {
        "loss": average_loss,
        "probabilities": probabilities,
        "labels": labels,
    }


def compute_metrics(
    labels: list[int],
    probabilities: list[float],
    threshold: float,
) -> JsonDict:
    tp = fp = tn = fn = 0
    for label, probability in zip(labels, probabilities):
        prediction = 1 if probability >= threshold else 0
        if prediction == 1 and label == 1:
            tp += 1
        elif prediction == 1 and label == 0:
            fp += 1
        elif prediction == 0 and label == 0:
            tn += 1
        else:
            fn += 1

    total = tp + fp + tn + fn
    accuracy = (tp + tn) / total if total else 0.0
    true_positive_rate = tp / (tp + fn) if (tp + fn) else 0.0
    true_negative_rate = tn / (tn + fp) if (tn + fp) else 0.0
    balanced_accuracy = (true_positive_rate + true_negative_rate) / 2.0
    precision = tp / (tp + fp) if (tp + fp) else 0.0
    recall = true_positive_rate
    return {
        "threshold": threshold,
        "sample_count": total,
        "accuracy": accuracy,
        "balanced_accuracy": balanced_accuracy,
        "precision": precision,
        "recall": recall,
        "true_positive": tp,
        "false_positive": fp,
        "true_negative": tn,
        "false_negative": fn,
    }


def choose_threshold(
    labels: list[int],
    probabilities: list[float],
    initial_threshold: float,
) -> JsonDict:
    candidates = [round(value / 100, 2) for value in range(5, 100, 5)]
    best_metrics = compute_metrics(labels, probabilities, initial_threshold)
    for candidate in candidates:
        candidate_metrics = compute_metrics(labels, probabilities, candidate)
        is_better = (
            candidate_metrics["balanced_accuracy"]
            > best_metrics["balanced_accuracy"]
        )
        same_score = (
            candidate_metrics["balanced_accuracy"]
            == best_metrics["balanced_accuracy"]
        )
        closer_to_default = abs(
            candidate - initial_threshold
        ) < abs(best_metrics["threshold"] - initial_threshold)
        if is_better or (same_score and closer_to_default):
            best_metrics = candidate_metrics
    return best_metrics


def metrics_with_loss(
    prediction: JsonDict,
    threshold: float,
) -> JsonDict:
    return {
        **compute_metrics(
            prediction["labels"],
            prediction["probabilities"],
            threshold,
        ),
        "loss": prediction["loss"],
    }


def train_model(
    splits: dict[str, list[dict[str, str]]],
    dataset_root: Path,
    config: PipelineConfig,
    runtime: RuntimeModules,
    run_dir: Path,
) -> dict[str, Any]:
    torch = runtime.torch
    nn = runtime.nn
    data_loader = runtime.DataLoader
    dataset_class = build_dataset_class(runtime)
    device = choose_device(runtime, config.device)
    model = build_model(runtime).to(device)

    datasets = {
        split_name: dataset_class(split_rows, config.image_size, dataset_root)
        for split_name, split_rows in splits.items()
    }
    loaders = {
        "train": build_data_loader(
            data_loader,
            datasets["train"],
            config.batch_size,
            shuffle=True,
        ),
        "val": build_data_loader(
            data_loader,
            datasets["val"],
            config.batch_size,
            shuffle=False,
        ),
        "test": build_data_loader(
            data_loader,
            datasets["test"],
            config.batch_size,
            shuffle=False,
        ),
    }

    criterion = nn.CrossEntropyLoss()
    optimizer = torch.optim.Adam(model.parameters(), lr=config.learning_rate)
    best_state: dict[str, Any] | None = None
    best_threshold = config.initial_threshold
    best_score = -1.0
    history: list[dict[str, Any]] = []

    torch.manual_seed(config.seed)
    random.seed(config.seed)

    for epoch in range(1, config.epochs + 1):
        model.train()
        train_loss_total = 0.0
        train_count = 0
        for images, labels in loaders["train"]:
            images = images.to(device)
            labels = labels.to(device)
            optimizer.zero_grad()
            logits = model(images)
            loss = criterion(logits, labels)
            loss.backward()
            optimizer.step()
            batch_size = int(labels.size(0))
            train_loss_total += float(loss.item()) * batch_size
            train_count += batch_size

        val_prediction = collect_predictions(
            model, loaders["val"], runtime, device, criterion)
        threshold_metrics = choose_threshold(
            val_prediction["labels"],
            val_prediction["probabilities"],
            config.initial_threshold,
        )

        epoch_record = {
            "epoch": epoch,
            "train_loss": (
                train_loss_total / train_count if train_count else 0.0
            ),
            "val_loss": val_prediction["loss"],
            "val_balanced_accuracy": threshold_metrics["balanced_accuracy"],
            "selected_threshold": threshold_metrics["threshold"],
        }
        history.append(epoch_record)

        if threshold_metrics["balanced_accuracy"] > best_score:
            best_score = threshold_metrics["balanced_accuracy"]
            best_threshold = float(threshold_metrics["threshold"])
            best_state = {name: tensor.detach().cpu()
                          for name, tensor in model.state_dict().items()}

    if best_state is None:
        raise PipelineError("学習済みモデルを保存できませんでした")

    model.load_state_dict(best_state)
    checkpoint_path = run_dir / "checkpoints" / "best_model.pt"
    checkpoint_path.parent.mkdir(parents=True, exist_ok=True)
    torch.save(best_state, checkpoint_path)

    val_prediction = collect_predictions(
        model, loaders["val"], runtime, device, criterion)
    test_prediction = collect_predictions(
        model, loaders["test"], runtime, device, criterion)
    train_prediction = collect_predictions(
        model, loaders["train"], runtime, device, criterion)

    metrics = {
        "device": device,
        "history": history,
        "train": metrics_with_loss(train_prediction, best_threshold),
        "val": metrics_with_loss(val_prediction, best_threshold),
        "test": metrics_with_loss(test_prediction, best_threshold),
    }
    return {
        "model": model,
        "metrics": metrics,
        "threshold": best_threshold,
        "checkpoint_path": str(checkpoint_path),
        "device": device,
    }


def export_onnx(
    model: Any,
    runtime: RuntimeModules,
    config: PipelineConfig,
    run_dir: Path,
) -> Path:
    torch = runtime.torch
    onnx_path = run_dir / "onnx" / "model.onnx"
    onnx_path.parent.mkdir(parents=True, exist_ok=True)
    dummy_input = torch.randn(1, 3, config.image_size, config.image_size)
    model.eval()
    torch.onnx.export(
        model.cpu(),
        dummy_input,
        onnx_path,
        input_names=["input"],
        output_names=["logits"],
        opset_version=17,
    )
    return onnx_path


def run_espdl_conversion(
    command_template: str | None,
    onnx_path: Path,
    run_dir: Path,
) -> JsonDict:
    if not command_template:
        return {
            "requested": False,
            "status": "skipped",
            "message": "ESP-DL 変換コマンドが未指定のため ONNX 生成までで終了しました",
        }

    espdl_path = run_dir / "espdl" / "model.espdl"
    espdl_path.parent.mkdir(parents=True, exist_ok=True)
    command = command_template.format(
        onnx_path=str(onnx_path), espdl_path=str(espdl_path))
    completed = subprocess.run(
        shlex.split(command),
        capture_output=True,
        text=True,
        check=False,
    )
    conversion_result = {
        "requested": True,
        "status": "success" if completed.returncode == 0 else "failed",
        "command": command,
        "returncode": completed.returncode,
        "stdout": completed.stdout,
        "stderr": completed.stderr,
        "espdl_path": str(espdl_path),
    }
    if completed.returncode != 0:
        raise PipelineError("ESP-DL 変換コマンドが失敗しました")
    return conversion_result


def run_pipeline(config: PipelineConfig) -> Path:
    runtime = load_runtime_modules()
    dataset_root = Path(config.dataset_root)
    metadata_path = Path(config.metadata_path)
    run_dir = Path(config.output_root) / config.run_name
    run_dir.mkdir(parents=True, exist_ok=True)

    write_json(run_dir / "config.json", asdict(config))

    rows = read_metadata(metadata_path)
    audit = audit_dataset(rows, dataset_root, runtime)
    write_json(run_dir / "dataset_audit.json", audit)
    if audit["errors"]:
        raise PipelineError("データ監査で致命的エラーが見つかりました")

    training_rows = select_training_rows(rows)
    splits = split_rows_by_subject(training_rows, config)
    for split_name, split_rows in splits.items():
        write_split_csv(run_dir / "splits" / f"{split_name}.csv", split_rows)

    training_result = train_model(
        splits, dataset_root, config, runtime, run_dir)
    write_json(
        run_dir / "reports" / "metrics.json",
        training_result["metrics"],
    )
    write_json(
        run_dir / "reports" / "threshold.json",
        {
            "threshold": training_result["threshold"],
            "selection_split": "val",
            "class_order": CLASS_NAMES,
        },
    )

    onnx_path = export_onnx(training_result["model"], runtime, config, run_dir)
    conversion_result = run_espdl_conversion(
        config.espdl_converter_command, onnx_path, run_dir)
    write_json(run_dir / "reports" / "conversion.json", conversion_result)
    return run_dir


def main() -> None:
    try:
        config = build_config(parse_args())
        run_dir = run_pipeline(config)
    except PipelineError as error:
        raise SystemExit(str(error))

    print(f"PC モデル生成が完了しました: {run_dir}")
