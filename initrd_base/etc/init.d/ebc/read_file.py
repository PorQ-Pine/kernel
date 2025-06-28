#!/usr/bin/env python
# Same licence as
# https://github.com/smaeul/linux/blob/rk35/ebc-drm-v5/drivers/gpu/drm/ \
#   drm_epd_helper.c
# Author: Maximilian Weigand https://github.com/m-weigand/mw_pinenote_misc/tree/main/waveforms
import ctypes
import struct
# import matplotlib.pylab as plt

import numpy as np


class waveform_file(object):
    def __init__(self, filename):
        self.data = open(filename, 'rb').read()
        self.index = 0

        self._header = None
        self._lut_offsets = None

    def get(self, nr_bytes):
        """Return the next nr_bytes bytes"""
        subdata = self.data[self.index: self.index + nr_bytes]
        self.index += nr_bytes
        return subdata

    def header(self):
        if self._header is None:
            self._header = self._read_header()
        return self._header

    def _read_header(self):
        header = {
            'checksum': struct.unpack('<I', self.get(4))[0],
            'file_size': struct.unpack('<I', self.get(4))[0],
            'serial': struct.unpack('<I', self.get(4))[0],
            'run_type': struct.unpack('B', self.get(1))[0],
            'fpl_platform': struct.unpack('B', self.get(1))[0],
            # __le16          fpl_lot;        // 0x0e
            'fpl_lot': struct.unpack('<H', self.get(2))[0],
            # u8          mode_version;       // 0x10
            'mode_version': struct.unpack('B', self.get(1))[0],
            # u8          wf_version;     // 0x11
            'wf_version': struct.unpack('B', self.get(1))[0],
            # u8          wf_subversion;      // 0x12
            'wf_subversion': struct.unpack('B', self.get(1))[0],
            # u8          wf_type;        // 0x13
            'wf_type': struct.unpack('B', self.get(1))[0],
            # u8          panel_size;     // 0x14
            'panel_size': struct.unpack('B', self.get(1))[0],
            # u8          amepd_part_number;  // 0x15
            'amepd_part_number': struct.unpack('B', self.get(1))[0],
            # u8          wf_rev;         // 0x16
            'wf_rev': struct.unpack('B', self.get(1))[0],
            # u8          frame_rate_bcd;     // 0x17
            'frame_rate_bcd': struct.unpack('B', self.get(1))[0],
            # u8          frame_rate_hex;     // 0x18
            'frame_rate_hex': struct.unpack('B', self.get(1))[0],
            # u8          vcom_offset;        // 0x19
            'vcom_offset': struct.unpack('B', self.get(1))[0],
            # u8          unknown[2];     // 0x1a
            'unknown_1': struct.unpack('B', self.get(1))[0],
            'unknown_2': struct.unpack('B', self.get(1))[0],
            # struct pvi_wbf_offset   xwia;           // 0x1c
            # struct pvi_wbf_offset {
            #     u8          b[3];
            # };
            'xwia_1': struct.unpack('B', self.get(1))[0],
            'xwia_2': struct.unpack('B', self.get(1))[0],
            'xwia_3': struct.unpack('B', self.get(1))[0],
            # u8          cs1;            // 0x1f
            'cs1': struct.unpack('B', self.get(1))[0],
            # struct pvi_wbf_offset   wmta;           // 0x20
            'wmta_1': struct.unpack('B', self.get(1))[0],
            'wmta_2': struct.unpack('B', self.get(1))[0],
            'wmta_3': struct.unpack('B', self.get(1))[0],
            # u8          fvsn;           // 0x23
            'fvsn': struct.unpack('B', self.get(1))[0],
            # u8          luts;           // 0x24
            'luts': struct.unpack('B', self.get(1))[0],
            # u8          mode_count;     // 0x25
            # note: we need to add one to get the actual number of modes stored
            # in the file (apprently INIT is not counted here
            'mode_count': struct.unpack('B', self.get(1))[0] + 1,
            # u8          temp_range_count;   // 0x26
            # note: this one is also off by one
            'temp_range_count': struct.unpack('B', self.get(1))[0] + 1,
            # u8          advanced_wf_flags;  // 0x27
            'advanced_wf_flags': struct.unpack('B', self.get(1))[0],
            # u8          eb;         // 0x28
            'eb': struct.unpack('B', self.get(1))[0],
            # u8          sb;         // 0x29
            'sb': struct.unpack('B', self.get(1))[0],
            # u8          reserved[5];        // 0x2a
            'reserved_1': struct.unpack('B', self.get(1))[0],
            'reserved_2': struct.unpack('B', self.get(1))[0],
            'reserved_3': struct.unpack('B', self.get(1))[0],
            'reserved_4': struct.unpack('B', self.get(1))[0],
            'reserved_5': struct.unpack('B', self.get(1))[0],
            # u8          cs2;            // 0x2f
            'cs2': struct.unpack('B', self.get(1))[0],
            # u8          temp_range_table[]; // 0x30
            # TODO
        }
        # N temperature ranges require N+1 delimiters
        nr_temps = header['temp_range_count'] + 1
        header['temperatures'] = struct.unpack(
            '{}B'.format(nr_temps), self.get(nr_temps)
        )
        return header

    def get_lut_offsets(self):
        if self._lut_offsets is None:
            self._lut_offsets = self._get_lut_offsets()
        return self._lut_offsets

    def _get_lut_offsets(self):
        """

        """
        header = self.header()

        def get_offset(bits):
            return bits[0] | bits[1] << 8 | bits[2] << 16

        def check(bits):
            # checksum check of offset pointer
            return ctypes.c_ubyte(bits[0] + bits[1] + bits[2]).value == bits[3]

        # mode table
        offset = header[
            'wmta_1'
        ] | header['wmta_2'] << 8 | header['wmta_3'] << 16

        self._base_offset = offset

        lut_offsets = []

        mode_offsets = []
        for i in range(0, header['mode_count']):
            mode_table_offset = struct.unpack(
                '4B', self.data[offset:offset + 4])
            assert check(
                mode_table_offset
            ), 'mode {} problem {}'.format(i, mode_table_offset)
            print(mode_table_offset, get_offset(mode_table_offset[0:3]))
            # mode_offsets += [get_offset(mode_table_offset[0:3])]
            # extract temperature table
            temp_table = get_offset(mode_table_offset[0:3])
            print('temp_table offset', temp_table)
            mode_offsets += [temp_table]
            lut_offsets.append({})
            for j in range(0, header['temp_range_count']):
                temp_offset = struct.unpack(
                    '4B', self.data[temp_table:temp_table + 4]
                )
                assert check(
                    temp_offset
                ), 'temp problem: {} {} {}'.format(i, j, temp_offset)
                print('    ', temp_offset, get_offset(temp_offset[0:3]))
                lut_offsets[-1][header['temperatures'][j]] = get_offset(
                    temp_offset[0:3]
                )

                temp_table += 4
            offset += 4
        self._mode_offsets = mode_offsets
        return lut_offsets

    def decode_data(self, subdata):
        """Decode compressed data. Do not split the bytes into phase
        polarisation information.
        """
        token_repeat = int('fc', 16)
        token_end = int('ff', 16)

        index = 0
        data_long = []
        repeat_mode = True
        while index < len(subdata):
            token = subdata[index]
            if token == token_end:
                index += 1
                break

            # print(index, data)
            if token == token_repeat:
                repeat_mode = not repeat_mode
                index += 1
                token = subdata[index]

            if repeat_mode:
                nr = subdata[index + 1]
                index = index + 1
                for i in range(nr + 1):
                    data_long += [token]
            else:
                data_long += [token]
            index += 1

        return data_long, index

    def split_bytes_into_polarisations(self, byte_data):
        polarisations = []
        for a in byte_data:
            p1 = a & 3
            p2 = (a >> 2) & 3
            p3 = (a >> 4) & 3
            p4 = (a >> 6) & 3
            polarisations += [p1, p2, p3, p4]
        return polarisations

    def encode_long_data(self, data_long):
        # re-encode a long byte stream

        token_repeat = int('fc', 16)
        token_end = int('ff', 16)
        data_rec = []
        index = 0

        def look_ahead(data, index_start):
            if data[index_start] == data[index_start + 1]:
                repeat = True
                index = index_start
                while(
                        index < len(data) - 1 and
                        data[index] == data[index + 1]):
                    index += 1
                return repeat, index - index_start
            else:
                repeat = False
                index = index_start
                while(
                        index < len(data) - 1 and
                        data[index] != data[index + 1]):
                    index += 1
                return repeat, index - index_start

        # tests
        # print('look 1', look_ahead([1, 1, 1, 2, 3, 4], 0))
        # print('look 2', look_ahead([1, 1, 2, 3, 4, 4], 0))
        # print('look 3', look_ahead([1, 1, 2, 3, 4, 4], 1))

        repeat_mode = True
        while(index < len(data_long[0:])):
            repeat, count = look_ahead(data_long, index)
            # print('look ahead', repeat, count)

            # we need to decide if its worthwhile to change the state
            if not repeat and repeat_mode:
                print('Evaluating state')
                if count > 1:
                    print('   deactivating at index', index, len(data_rec))
                    repeat_mode = False
                    data_rec += [token_repeat]

            if repeat and not repeat_mode:
                if count >= 2:
                    print('   activating at index', index, len(data_rec))
                    repeat_mode = True
                    data_rec += [token_repeat]

            # do we have repeating entries
            if repeat:
                if repeat_mode:
                    counts = [255] * int(count / 255) + [count % 255]
                    for subcount in counts:
                        data_rec += [data_long[index], subcount]
                else:
                    for i in range(1 + count):
                        data_rec += [data_long[index]]

                index += count
            else:
                # no repetition ahead

                # do we need to switch?
                if repeat_mode:
                    data_rec += [data_long[index], 0]
                else:
                    data_rec += [data_long[index]]
            index += 1
            # break
            # if len(data_rec) > 115:
            #     break
        data_rec += [token_end]

        # check
        # for nr, (orig, rec) in enumerate(zip(data_orig, data_rec)):
        #     if orig != rec:
        #         print('Error at index: ', nr)

        # print(list(data_orig[len(data_rec) - 10: len(data_rec) + 10]))
        # print(data_rec[-10:])
        # print(data_long[index - 10: index + 10])
        return data_rec

    def get_lut(
            self, wf, temperature, split_bytes=True, return_compressed=False):
        """Return the decompressed 1D lut as stored in the file (i.e. 32 x 32
        levels).

        Parameters
        ----------
        wf : int
            Index of waveform. Must lie in range [0:header['mode_count']]
        temp: int
            Temperature to retrieve lut at. See header['temperatures'] for
            valid values
        split_bytes: bool, True
            If true, split each byte so we get individual polarisation values
        return_compressed: bool, False
            if True, also return the compressed byte stream of the lut

        Returns
        -------
        """
        header = self.header()
        assert wf in list(range(0, header['mode_count'])), \
            "wf index out of range"
        assert temperature in header['temperatures'], \
            "temperature not valid"

        lut_offsets = self.get_lut_offsets()
        # could be more restricted
        subdata = self.data[lut_offsets[wf][temperature]:]
        data_long, index = self.decode_data(subdata)
        data_orig = subdata[:index]
        if split_bytes:
            data_long = self.split_bytes_into_polarisations(data_long)
        if return_compressed:
            return data_long, data_orig
        return data_long

