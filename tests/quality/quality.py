#!/usr/bin/env python3
"""Reproducible real-speech corpus preparation and strict ASR scoring (stdlib)."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import array
import hashlib
import json
import math
from pathlib import Path
import random
import re
import subprocess
import sys
import unicodedata
import wave

SOURCE = 'https://huggingface.co/datasets/google/fleurs'
REVISION = '70bb2e84b976b7e960aa89f1c648e09c59f894dd'


def read_jsonl(path):
    items = [json.loads(line) for line in Path(path).read_text().splitlines() if line.strip()]
    ids = [item['id'] for item in items]
    if len(ids) != len(set(ids)):
        raise ValueError(f'Duplicate case IDs: {path}')
    return items


def write_jsonl(path, rows):
    Path(path).write_text(''.join(json.dumps(r, ensure_ascii=False) + '\n' for r in rows))


def download(url, path):
    temporary = Path(str(path) + '.part')
    subprocess.run(['curl', '-fLsS', '--retry', '2', '--max-time', '55', url, '-o', str(temporary)], check=True)
    temporary.replace(path)


def read_wav(path):
    with wave.open(str(path), 'rb') as audio:
        if (audio.getnchannels(), audio.getsampwidth(), audio.getframerate()) != (1, 2, 16000):
            raise ValueError(f'Expected mono PCM16 16 kHz: {path}')
        pcm = array.array('h', audio.readframes(audio.getnframes()))
    if sys.byteorder != 'little':
        pcm.byteswap()
    return pcm


def write_wav(path, samples):
    pcm = array.array('h', (round(max(-32768, min(32767, x))) for x in samples))
    if sys.byteorder != 'little':
        pcm.byteswap()
    with wave.open(str(path), 'wb') as audio:
        audio.setnchannels(1)
        audio.setsampwidth(2)
        audio.setframerate(16000)
        audio.writeframes(pcm.tobytes())


def mix_noise(pcm, snr, seed):
    # Controlled stationary white-noise stress, NOT a real café recording.
    rng = random.Random(seed)
    noise = [rng.gauss(0, 1) for _ in pcm]
    rms = math.sqrt(sum(x*x for x in pcm) / len(pcm))
    noise_rms = math.sqrt(sum(x*x for x in noise) / len(noise))
    scale = rms / (10 ** (snr / 20)) / noise_rms
    mixed = [x + n * scale for x, n in zip(pcm, noise)]
    attenuation = min(1, 30000 / max(1, max(abs(x) for x in mixed)))
    return [x * attenuation for x in mixed]


def prepare(root):
    root = Path(root).resolve()
    root.mkdir(parents=True, exist_ok=True)
    rows = []
    seen = set()
    for language, count in [('cmn_hans_cn', 10), ('en_us', 5)]:
        for split, fold in [('validation', 'dev'), ('test', 'holdout')]:
            index = root / f'{language}-{split}.json'
            if not index.exists():
                url = ('https://datasets-server.huggingface.co/rows?dataset=google%2Ffleurs'
                       f'&config={language}&split={split}&offset=0&length=100')
                download(url, index)
            candidates = json.loads(index.read_text())['rows']
            selected = []
            for entry in candidates:
                row = entry['row']
                key = (language, row['id'])
                if key in seen:
                    continue
                seen.add(key)
                selected.append(entry)
                if len(selected) == count:
                    break
            if len(selected) != count:
                raise ValueError('Insufficient unique source utterances')
            def fetch_audio(entry):
                row = entry['row']
                stem = f'fleurs-{language}-{split}-{entry["row_idx"]}'
                if not (root / f'{stem}.wav').exists():
                    src = row['audio'][0]['src']
                    if REVISION not in src:
                        raise ValueError('Dataset revision changed')
                    download(src, root / f'{stem}.download.wav')
            with ThreadPoolExecutor(max_workers=4) as pool:
                list(pool.map(fetch_audio, selected))
            for entry in selected:
                row = entry['row']
                stem = f'fleurs-{language}-{split}-{entry["row_idx"]}'
                target = root / f'{stem}.wav'
                if not target.exists():
                    raw = root / f'{stem}.download.wav'
                    src = row['audio'][0]['src']
                    if REVISION not in src:
                        raise ValueError('Dataset revision changed; review and pin new revision')
                    if not raw.exists():
                        download(src, raw)
                    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', str(raw), '-ar', '16000',
                                    '-ac', '1', '-c:a', 'pcm_s16le', str(target)], check=True)
                    raw.unlink()
                pcm = read_wav(target)
                reference = row['raw_transcription']
                base = dict(id=stem, audio=str(target), reference=reference, fold=fold,
                            category=f'{language}/clean', source=SOURCE, revision=REVISION,
                            source_id=row['id'], source_row=entry['row_idx'], source_split=split,
                            gender=row['gender'], license='CC-BY-4.0', audio_kind='human_read_speech')
                base['sha256'] = hashlib.sha256(target.read_bytes()).hexdigest()
                rows.append(base)
                for snr in (20, 10):
                    noisy = root / f'{stem}-white-{snr}db.wav'
                    seed = int(hashlib.sha256(stem.encode()).hexdigest()[:8], 16)
                    write_wav(noisy, mix_noise(pcm, snr, seed))
                    case = dict(base, id=f'{stem}-white-{snr}db', audio=str(noisy),
                                category=f'{language}/white-{snr}db',
                                audio_kind='human_speech_with_synthetic_noise', snr_db=snr, seed=seed)
                    case['sha256'] = hashlib.sha256(noisy.read_bytes()).hexdigest()
                    rows.append(case)
                print(stem, flush=True)
    # Long utterances reuse sources ONLY within their fold. These are derived
    # stress tests, not independent people or spontaneous dictation samples.
    for fold in ('dev', 'holdout'):
        for language in ('cmn_hans_cn', 'en_us'):
            selected = [r for r in rows if r['fold'] == fold and r['category'] == language+'/clean']
            samples, references, source_ids = [], [], []
            for case in selected:
                samples.extend(read_wav(case['audio']))
                samples.extend([0]*8000)
                references.append(case['reference'])
                source_ids.append(case['id'])
                if len(samples) >= 16000*65:
                    break
            stem = f'long-{language}-{fold}'
            target = root / f'{stem}.wav'
            write_wav(target, samples)
            rows.append(dict(id=stem, audio=str(target), reference=' '.join(references), fold=fold,
                             category=language+'/long', source=SOURCE, revision=REVISION,
                             license='CC-BY-4.0', audio_kind='concatenated_human_speech',
                             source_ids=source_ids, sha256=hashlib.sha256(target.read_bytes()).hexdigest()))
    for name, samples in [('silence', [0]*80000),
                          ('white-no-speech', [random.Random(i).gauss(0, 500) for i in range(80000)])]:
        target = root / f'{name}.wav'
        write_wav(target, samples)
        rows.append(dict(id=name, audio=str(target), reference='', fold='dev', category='no-speech',
                         audio_kind='synthetic_no_speech', sha256=hashlib.sha256(target.read_bytes()).hexdigest()))
    write_jsonl(root/'manifest.jsonl', rows)
    for fold in ('dev', 'holdout'):
        write_jsonl(root/f'{fold}.jsonl', [r for r in rows if r['fold'] == fold])
    print(f'{len(rows)} cases; corpus: {root}', flush=True)


def normalize(text, punctuation=False):
    text = unicodedata.normalize('NFKC', text).casefold()
    return ''.join(c for c in text if not c.isspace() and
                   (punctuation or not unicodedata.category(c).startswith(('P', 'S'))))


def tokens(text):
    text = unicodedata.normalize('NFKC', text).casefold()
    return re.findall(r'[a-z0-9]+(?:\x27[a-z0-9]+)*|[\u3400-\u9fff]|[^\W_]', text)


def edits(reference, hypothesis):
    previous = list(range(len(hypothesis)+1))
    for i, ref in enumerate(reference, 1):
        current = [i]
        for j, hyp in enumerate(hypothesis, 1):
            current.append(min(previous[j]+1, current[j-1]+1, previous[j-1]+(ref != hyp)))
        previous = current
    return previous[-1]


def percentile(values, p):
    if not values:
        return None
    return sorted(values)[max(0, math.ceil(len(values)*p)-1)]


def score(manifest, results):
    cases = read_jsonl(manifest)
    outputs = {r['id']: r for r in read_jsonl(results)}
    expected = {r['id'] for r in cases}
    if set(outputs) != expected:
        raise ValueError(f'Incomplete/mismatched results: missing={sorted(expected-set(outputs))}, '
                         f'extra={sorted(set(outputs)-expected)}')
    details, groups = [], {}
    for case in cases:
        result = outputs[case['id']]
        if 'error' in result or not isinstance(result.get('text'), str):
            raise ValueError(f'Failed recognition: {case["id"]}')
        ref, hyp = normalize(case['reference']), normalize(result['text'])
        wt, wh = tokens(case['reference']), tokens(result['text'])
        row = dict(id=case['id'], category=case['category'], fold=case['fold'],
                   reference=case['reference'], text=result['text'],
                   char_edits=edits(ref, hyp), ref_chars=len(ref),
                   token_edits=edits(wt, wh), ref_tokens=len(wt),
                   raw_edits=edits(normalize(case['reference'], True), normalize(result['text'], True)),
                   raw_chars=len(normalize(case['reference'], True)),
                   false_insertion=not ref and bool(hyp),
                   finish_ms=result.get('finish_ms'), init_ms=result.get('init_ms'))
        details.append(row)
        groups.setdefault(f'{case["fold"]}/{case["category"]}', []).append(row)
    summary = {}
    for key, group in groups.items():
        chars = sum(r['ref_chars'] for r in group)
        words = sum(r['ref_tokens'] for r in group)
        raw_chars = sum(r['raw_chars'] for r in group)
        summary[key] = dict(cases=len(group), cer=sum(r['char_edits'] for r in group)/chars if chars else None,
                            mixed_token_error_rate=sum(r['token_edits'] for r in group)/words if words else None,
                            raw_error_rate=sum(r['raw_edits'] for r in group)/raw_chars if raw_chars else None,
                            false_insertions=sum(r['false_insertion'] for r in group),
                            finish_p50_ms=percentile([r['finish_ms'] for r in group if r['finish_ms'] is not None], .5),
                            finish_p95_ms=percentile([r['finish_ms'] for r in group if r['finish_ms'] is not None], .95))
    return dict(summary=summary, cases=details)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    prep = sub.add_parser('prepare')
    prep.add_argument('directory')
    scoring = sub.add_parser('score')
    scoring.add_argument('manifest')
    scoring.add_argument('results')
    scoring.add_argument('output')
    args = parser.parse_args()
    if args.command == 'prepare':
        prepare(args.directory)
    else:
        report = score(args.manifest, args.results)
        Path(args.output).write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n')
        print(json.dumps(report['summary'], ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
