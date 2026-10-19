"""Trains the light voice on a rented GPU machine (train_colab.ipynb without
Drive): the data from dataset.zip or dataset-flac.zip in /workspace/data,
our own symbol table, a warm start from a Piper base checkpoint (or a resume
of this run), then the ONNX export and the listening samples.

    source /workspace/venv-piper/bin/activate
    python train_light.py --run pilot --base he_IL-saspeech --epochs 200 [--zip /workspace/data/dataset-flac.zip]
    python train_light.py --run pilot --export-only

Output: /workspace/runs/<run>/{phonemes.json, chaim-light.json, ckpt/, chaim-light.onnx, samples/}.
Bases: en_US-ljspeech (LJ Speech: public domain, trained from scratch) for anything published;
he_IL-saspeech (non-commercial) and en_US-lessac (Blizzard 2013 data: research use only) only
for experiments.
"""
import argparse
import csv
import glob
import json
import os
import subprocess
import sys
import unicodedata
import wave
import zipfile


def sh(cmd, check=True):
    print("+", cmd, flush=True)
    return subprocess.run(cmd, shell=True, check=check)


def prepare_data(zip_path, data_dir):
    if os.path.exists(os.path.join(data_dir, "metadata.csv")):
        return
    os.makedirs(data_dir, exist_ok=True)
    with zipfile.ZipFile(zip_path) as z:
        z.extractall(data_dir)
    flacs = glob.glob(os.path.join(data_dir, "wavs", "*.flac"))
    if flacs:
        import soundfile as sf
        for f in flacs:
            x, rate = sf.read(f, dtype="int16")
            sf.write(f[:-5] + ".wav", x, rate, subtype="PCM_16")
            os.remove(f)
        print(len(flacs), "flac -> wav", flush=True)


def symbol_table(data_dir, path):
    if os.path.exists(path):
        return json.load(open(path, encoding="utf-8"))
    chars = set()
    with open(os.path.join(data_dir, "metadata.csv"), encoding="utf-8") as f:
        for row in csv.reader(f, delimiter="|"):
            chars.update(unicodedata.normalize("NFD", row[-1]))
    chars -= {"_", "^", "$"}
    id_map = {"_": [0], "^": [1], "$": [2]}
    for i, ch in enumerate(sorted(chars), start=3):
        id_map[ch] = [i]
    assert len(id_map) <= 256, len(id_map)
    json.dump(id_map, open(path, "w", encoding="utf-8"), ensure_ascii=False)
    return id_map


def base_checkpoint(base):
    from huggingface_hub import hf_hub_download, list_repo_files
    folder = {"he_IL-saspeech": "he/he_IL/saspeech/medium", "en_US-lessac": "en/en_US/lessac/medium",
              "en_US-ljspeech": "en/en_US/ljspeech/medium"}[base]
    files = [f for f in list_repo_files("rhasspy/piper-checkpoints", repo_type="dataset")
             if f.startswith(folder + "/") and f.endswith(".ckpt")]
    return hf_hub_download("rhasspy/piper-checkpoints", files[0], repo_type="dataset", local_dir="/workspace/models")


def export_and_listen(out, data_dir):
    import numpy as np
    import onnxruntime as ort
    last = sorted(glob.glob(f"{out}/ckpt/*.ckpt"), key=os.path.getmtime)[-1]
    sh(f'python3 -m piper.train.export_onnx --checkpoint "{last}" --output-file {out}/chaim-light.onnx')
    cfg = json.load(open(f"{out}/chaim-light.json", encoding="utf-8"))
    # The speaker chose these (18-19.10): less noise reads cleaner and sharper; 1.0 spoke ~25%
    # faster than Chatterbox and blurred words, 1.4 matches it.
    cfg["inference"] = {"noise_scale": 0.333, "length_scale": 1.4, "noise_w": 0.6}
    json.dump(cfg, open(f"{out}/chaim-light.json", "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    id_map = cfg["phoneme_id_map"]
    sess = ort.InferenceSession(f"{out}/chaim-light.onnx")

    def say(text):
        ids = list(id_map["^"]) + list(id_map["_"])  # Piper: bos, pad, (symbol, pad)*, eos
        for ch in unicodedata.normalize("NFD", text):
            if ch in id_map:
                ids += id_map[ch] + id_map.get("_", [0])
        ids += id_map.get("$", [])
        x = np.array([ids], dtype=np.int64)
        return sess.run(None, {"input": x, "input_lengths": np.array([x.shape[1]], dtype=np.int64),
                               "scales": np.array([cfg["inference"]["noise_scale"], cfg["inference"]["length_scale"], cfg["inference"]["noise_w"]], dtype=np.float32)})[0].squeeze()

    os.makedirs(f"{out}/samples", exist_ok=True)
    rate = cfg["audio"]["sample_rate"]
    for i, line in enumerate(open(os.path.join(data_dir, "test_sentences.txt"), encoding="utf-8")):
        pcm = say(line.strip())
        with wave.open(f"{out}/samples/{i:02d}.wav", "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes((np.clip(pcm, -1, 1) * 32767).astype(np.int16).tobytes())
    print("samples in", f"{out}/samples", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", default="pilot")
    ap.add_argument("--base", default="en_US-ljspeech")
    ap.add_argument("--epochs", type=int, default=200)
    ap.add_argument("--batch", type=int, default=32)
    ap.add_argument("--zip", default=None, help="dataset.zip or dataset-flac.zip (default: the one in /workspace/data)")
    ap.add_argument("--data", default="/workspace/data/set")
    ap.add_argument("--export-only", action="store_true")
    ap.add_argument("--devices", type=int, default=1, help="GPUs (DDP); the batch is per GPU")
    args = ap.parse_args()
    out = f"/workspace/runs/{args.run}"
    os.makedirs(f"{out}/ckpt", exist_ok=True)
    zip_path = args.zip or next(iter(glob.glob("/workspace/data/dataset*.zip")), None)
    if not os.path.exists(os.path.join(args.data, "metadata.csv")):
        if not zip_path:
            sys.exit("no dataset zip in /workspace/data")
        prepare_data(zip_path, args.data)
    if args.export_only:
        export_and_listen(out, args.data)
        return
    phonemes = f"{out}/phonemes.json"
    table = symbol_table(args.data, phonemes)
    print(len(table), "symbols", flush=True)
    ckpts = sorted(glob.glob(f"{out}/ckpt/*.ckpt"), key=os.path.getmtime)
    start = f'--ckpt_path "{ckpts[-1]}"' if ckpts else f'--model.warmstart_ckpt "{base_checkpoint(args.base)}"'
    print("starting with", start, flush=True)
    multi = (f"--trainer.devices {args.devices} --trainer.strategy ddp_find_unused_parameters_true"
             if args.devices > 1 else "")
    sh(f"""python3 -m piper.train fit \
  --data.voice_name chaim-light \
  --data.csv_path {args.data}/metadata.csv \
  --data.audio_dir {args.data}/wavs \
  --data.phoneme_type text \
  --data.espeak_voice he \
  --data.phonemes_path {phonemes} \
  --model.sample_rate 22050 \
  --data.cache_dir /workspace/cache \
  --data.config_path {out}/chaim-light.json \
  --data.batch_size {args.batch} \
  --data.num_workers 4 \
  --trainer.max_epochs {args.epochs} \
  {multi} \
  --trainer.default_root_dir /workspace/lightning \
  --trainer.callbacks+=ModelCheckpoint \
  --trainer.callbacks.dirpath {out}/ckpt \
  --trainer.callbacks.every_n_train_steps 500 \
  --trainer.callbacks.save_top_k 1 \
  {start}""")
    export_and_listen(out, args.data)


if __name__ == "__main__":
    main()
