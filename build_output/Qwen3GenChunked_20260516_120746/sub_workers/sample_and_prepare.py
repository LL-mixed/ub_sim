"""SubWorker: sample_and_prepare — auto-generated, callable as fn(args: TaskArgs)."""

import torch

from pypto.runtime.distributed_runner import _tensor_from_continuous


def _user_sample_and_prepare(logits_padded, decode_seq_lens, decode_slot_mapping, decode_hidden):
    pass


def sample_and_prepare(args):
    logits_padded = _tensor_from_continuous(args.tensor(0))
    decode_seq_lens = _tensor_from_continuous(args.tensor(1))
    decode_slot_mapping = _tensor_from_continuous(args.tensor(2))
    decode_hidden = _tensor_from_continuous(args.tensor(3))
    _user_sample_and_prepare(logits_padded, decode_seq_lens, decode_slot_mapping, decode_hidden)
