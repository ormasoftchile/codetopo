#!/usr/bin/env python3
"""
Generate 256d int8 token embedding table for codetopo semantic search.
Uses CodeRankEmbed (nomic-ai/CodeRankEmbed, Apache 2.0) at 256d.
Same tokenizer as nomic-embed-text-v1.5 — no changes needed in token_vectors.h split logic.

Run on M4 Mac (~5-10 min via MPS) or CUDA GPU (~1 min):
  /tmp/embed_env/bin/python3 tools/gen_token_vectors.py
"""

import struct
import numpy as np

MATRYOSHKA_DIM = 256
BATCH_SIZE = 512
MODEL_NAME = "nomic-ai/CodeRankEmbed"
OUT_BIN = "tools/token_vectors_256d.bin"
OUT_TOKENS = "tools/token_list.txt"

def main():
    from sentence_transformers import SentenceTransformer
    from transformers import AutoTokenizer

    print(f"Loading model: {MODEL_NAME}")
    model = SentenceTransformer(MODEL_NAME, trust_remote_code=True)

    import torch
    if torch.backends.mps.is_available():
        device = "mps"
    elif torch.cuda.is_available():
        device = "cuda"
    else:
        device = "cpu"
    model = model.to(device)
    print(f"Device: {device}")

    print("Loading tokenizer vocabulary...")
    tokenizer = AutoTokenizer.from_pretrained(MODEL_NAME)
    vocab = tokenizer.get_vocab()

    # Filter to code-relevant tokens
    tokens = []
    for tok in vocab.keys():
        clean = tok.replace("##", "").replace("▁", "").replace("Ġ", "")
        if any(c.isalnum() or c == "_" for c in clean):
            tokens.append(tok)

    print(f"Vocabulary: {len(vocab)} total -> {len(tokens)} code-relevant tokens")

    print(f"Embedding {len(tokens)} tokens at {MATRYOSHKA_DIM}d (batch={BATCH_SIZE})...")
    import time
    t0 = time.time()

    # CodeRankEmbed uses "Represent this query for searching relevant code: " prefix for queries,
    # but for document (symbol) embedding, use "Represent this code: " prefix
    prefixed = [f"Represent this code: {tok}" for tok in tokens]

    embeddings = model.encode(
        prefixed,
        batch_size=BATCH_SIZE,
        normalize_embeddings=True,
        show_progress_bar=True,
        device=device,
    )

    elapsed = time.time() - t0
    print(f"Done in {elapsed:.1f}s -- {len(tokens)/elapsed:.0f} tokens/sec")

    # Truncate to dim and re-normalize
    emb = embeddings[:, :MATRYOSHKA_DIM].astype(np.float32)
    norms = np.linalg.norm(emb, axis=1, keepdims=True)
    norms = np.where(norms == 0, 1.0, norms)
    emb = emb / norms

    # Quantize to int8
    emb_int8 = np.clip(np.round(emb * 127), -127, 127).astype(np.int8)

    print(f"Writing {OUT_BIN}...")
    with open(OUT_BIN, "wb") as f:
        f.write(struct.pack("<ii", len(tokens), MATRYOSHKA_DIM))
        f.write(emb_int8.tobytes())

    print(f"Writing {OUT_TOKENS}...")
    with open(OUT_TOKENS, "w", encoding="utf-8") as f:
        for tok in tokens:
            f.write(tok + "\n")

    size_mb = (8 + len(tokens) * MATRYOSHKA_DIM) / 1e6
    print(f"\nDone!")
    print(f"  Tokens:  {len(tokens):,}")
    print(f"  Dims:    {MATRYOSHKA_DIM}")
    print(f"  Size:    {size_mb:.1f} MB")
    print(f"  Output:  {OUT_BIN}")
    print(f"           {OUT_TOKENS}")

if __name__ == "__main__":
    main()
