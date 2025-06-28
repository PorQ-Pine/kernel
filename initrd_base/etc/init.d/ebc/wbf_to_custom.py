#!/usr/bin/env python3
import numpy as np
from pathlib import Path
from dataclasses import dataclass, field
import re
import sys
import struct
import enum
import read_file
import itertools

# [[max([len(s) for s in sm]) for sm in m.summaries] for m in list(modes.__dict__.values())]
# np.sum([[max([len(s) for s in sm]) for sm in m.summaries] for m in list(modes.__dict__.values())], 0)

SEQ_SHIFT = 6

class WfIdx(enum.Enum):
    DU = 0
    DU4 = 1
    GL16 = 2
    GC16 = 3
    INIT = 4
    IDLE = 5

class WF:
    def __repr__(self):
        return str(self.__dict__)

class Sim:
    outer: int
    inner: int
    i: bool
    phases: list[int]
    history: list[int]

    def __init__(self, wf):
        self.outer = 0
        self.inner = 0
        self.i = False
        self.phases = [0, 0]
        self.wf = wf
        self.history = []

    def schedule(self, src, dst, wf: WfIdx):
        # No conflict
        if self.inner == 0 and self.outer == 0:
            offset = self.wf.offsets[wf.value]
            t = self.wf.lut_array[src, dst, offset].item()
            self.outer = (offset & 0x7f) | (0x80 if (t & 0x3f) > 1 else 0x00)
            print(f'Scheduling {src=} {dst=} {offset=} {t=} {(t >> 6)=} {(t & 0x3f)=}')
            self.phases[self.i] = t >> 6
            self.inner = t

    def evolve(self, src, dst):
        inner_num = self.inner & 0x3f
        inner_num_ind = self.inner >> 6
        if self.outer == 0:
            return
        elif inner_num > 1 and self.outer & 0x80:
            # Handle double buffering
            self.phases[self.i] = inner_num_ind
            self.outer = self.outer & 0x7f
            self.inner -= 1
        elif inner_num == 1:
            self.outer = ((self.outer & 0x7f) + 1)
            t = self.wf.lut_array[src, dst, self.outer].item()
            self.outer |= 0x80
            self.phases[self.i] = t >> 6
            self.inner = t
        elif inner_num == 0:
            self.outer = 0
            self.phases[self.i] = 0
        else:
            self.inner -= 1
        print(self.outer, self.inner & 0x3f, self.phases[self.i])

    def frame(self):
        self.history.append(self.phases[self.i])
        self.i = not self.i

    def run(self, num_steps, src, dst, offset):
        self.outer = 0
        self.inner = 0
        self.phases[0] = 0
        self.phases[1] = 0
        self.history = []
        self.schedule(src, dst, offset)
        for i in range(num_steps):
            self.frame()
            self.evolve(src, dst)

Summary = tuple[tuple[int, int], list[tuple[int, int]]]

def summary_exceeds(summary: Summary, max_length: int) -> bool:
    return max([len(row[1]) for row in summary]) > max_length

class CustomWfTemp:
    temp_lower: int
    temp_upper: int
    seq_shift: int
    offsets: np.ndarray
    # lengths: list[int]
    lut_array = np.ndarray

    def __init__(self, modes: WF, temp_idx: int):
        _offset = 1
        self.offsets = np.zeros(len(WfIdx), np.uint8)
        for _wf in WfIdx:
            if _wf != WfIdx.IDLE:
                _lut_sum = getattr(modes, _wf.name).summaries[temp_idx]
                _max_len = max([len(row[1]) for row in _lut_sum])
                self.offsets[_wf.value] = _offset
                _offset += _max_len
            else:
                self.offsets[_wf.value] = _offset
        # Align for more efficient lookup
        # self.num_columns = 2 ** max(1, (int(np.log2(_offset - 0.9)) + 1))
        self.seq_shift = SEQ_SHIFT
        self.lut_array = np.zeros((16, 16, 1 << self.seq_shift), np.uint8)
        self.temp_lower = modes.INIT.temps[temp_idx]
        self.temp_upper = modes.INIT.temps[temp_idx + 1]
        for _wf, offset in zip(WfIdx, self.offsets):
            if _wf == WfIdx.IDLE:
                continue
            summary = getattr(modes, _wf.name).summaries[temp_idx]
            if summary_exceeds(summary, 64):
                raise ValueError("Value error")
            for ((src, dst), seq) in summary:
                for (i, (phase, repeat)) in enumerate(seq):
                    self.lut_array[src >> 1, dst >> 1, offset + i] = ((phase & 3) << 6) | (repeat & 0x1f)
                if seq:
                    self.lut_array[src >> 1, dst >> 1, offset + len(seq) - 1] |= 0x20

    def tobytes(self) -> bytes:
        header = struct.pack('ii', self.temp_lower, self.temp_upper)
        return header + self.offsets.tobytes('C') + self.lut_array.tobytes('C')

class CustomWf:
    luts: list[CustomWfTemp]

    def __init__(self, modes):
        temps = modes.INIT.temps.astype(int).tolist()
        self.luts = [CustomWfTemp(modes, temp_idx) for temp_idx in range(len(temps) - 1)]

    def tobytes(self):
        out = bytearray()
        header = b'CLUT0002' + struct.pack('I', len(self.luts))
        for lut in self.luts:
            out += lut.tobytes()
        return header + out

def table_summarise(tbl: np.ndarray):
    summaries = []
    for row in tbl:
        src, dst = row[:2].tolist()
        full_seq = row[2:]
        if full_seq.sum() > 0:
            summary = []
            current = -1
            count = 0
            for p in full_seq.tolist():
                # Remove prefix 0, which simply delays the sequence
                if (current != -1 and current != p) or count == 31:
                    summary.append((current, count))
                    count = 0
                if not summary and p == 0:
                    continue
                current = p
                count += 1
            if count > 0:
                summary.append((current, count))
            # This is a remove suffix function
            _last_idx = [x[0] for x in enumerate(reversed(summary)) if x[0] != 0][0]
            summary = summary[:-_last_idx]
            summaries.append(((src, dst), summary))
    return summaries

@dataclass
class Mode:
    name: str = ''
    fc: int = 0
    table_names: list[str] = field(default_factory=list)
    tables: list[np.ndarray] = field(default_factory=list, repr=False)
    summaries: list[list] = field(default_factory=list, repr=False)

def read_table(tname) -> np.ndarray:
    import pandas as pd
    data = pd.read_csv(tname, header=None).iloc[:,:-1].to_numpy()
    return data

def read_bin(fname) -> WF:
    wff = read_file.waveform_file(fname)
    header = wff.header()
    temps = np.array(header['temperatures'], np.int32)
    if (mode_version := header['mode_version']) != 0x19:
        raise ValueError(f'Unsupported mode version {mode_version}')
    modes = dict(INIT=0, DU=1, GC16=2, GL16=3, GLR16=4, GCC16=4, GLD16=5, A2=6, DU4=7)
    wf = WF()
    for mode_name, mode_idx in modes.items():
        mode = Mode()
        mode.name = mode_name
        mode.temps = temps
        for temp in temps[:-1]:
            subdata = wff.data[wff.get_lut_offsets()[mode_idx][temp]:]
            data_long, index = wff.decode_data(subdata)
            polarisations = np.array(wff.split_bytes_into_polarisations(data_long))
            polarisations = polarisations.reshape(-1, 32 * 32).T
            prev_next_prefix = np.array(list(itertools.product(range(32), range(32))))[:,::-1]
            tbl = np.hstack([prev_next_prefix, polarisations])
            mode.summaries.append(table_summarise(tbl))
            # np.allclose(modes.INIT.tables[12][:,2:].reshape(32, 32, -1), arr)
        setattr(wf, mode.name, mode)
    return wf

# This one is compatible with https://github.com/Modos-Labs/Glider/tree/main/utils/wbf_waveform_dump
def read_iwf(fname) -> WF:
    fname = Path(fname)
    iwf = fname.read_text().splitlines()

    mode = None
    temps = None
    prefix = ''
    wf = WF()
    for line in iwf:
        line = line.strip()
        if not line:
            continue

        if line.startswith('[MODE'):
            if mode is not None:
                mode.temps = temps
                setattr(wf, mode.name, mode)
            mode = Mode()
            continue
        elif line.startswith('['):
            continue

        k, _, v = line.split()
        if k == 'PREFIX':
            prefix = v
        elif k == 'TEMPS':
            temps = np.zeros(int(v) + 1, np.int32)
        elif k.endswith('RANGE'):
            temps[int(re.match(r'T(\d+)RANGE', k).group(1))] = int(v)
        elif k == 'TUPBOUND':
            temps[-1] = int(v)
        elif k.endswith('FC'):
            pass
        elif k == 'NAME' and mode is not None:
            mode.name = v
        elif k.endswith('TABLE'):
            mode.table_names.append(tname := f'{prefix}_TB{v}.csv')
            mode.tables.append(tbl := read_table(fname.parent / tname))
            mode.summaries.append(table_summarise(tbl))
    if mode is not None:
        mode.temps = temps
        setattr(wf,  mode.name, mode)
    return wf

def main():
    modes = read_bin(sys.argv[1])
    custom_wf = CustomWf(modes)
    # sim = Sim(custom_wf.luts[-1])
    Path('custom_wf.bin').write_bytes(custom_wf.tobytes())

if __name__ == "__main__":
    main()




