# Sigrok decoder for SCSI logic captures
#
# ZuluSCSI™ - Copyright (c) 2022-2025 Rabbit Hole Computing™
# 
# ZuluSCSI™ firmware is licensed under the GPL version 3 or any later version. 
# 
# https://www.gnu.org/licenses/gpl-3.0.html
# ----
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version. 
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details. 
# 
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.


import sigrokdecode as srd
from common.srdhelper import bitpack

class ChannelError(Exception):
    pass


# Category: 0 = control, 1 = read, 2 = write, 3 = special
SCSI_COMMANDS = {
    0x00: (0, "TestUnitReady"),
    0x01: (0, "RezeroUnit/Rewind"),
    0x03: (0, "RequestSense"),
    0x04: (0, "FormatUnit"),
    0x05: (0, "ReadBlockLimits"),
    0x06: (3, "IomegaVendorCommand"),
    0x08: (1, "Read6"),
    0x0A: (2, "Write6"),
    0x0B: (1, "Seek6"),
    0x0C: (3, "Xebec InitializeDriveCharacteristics"),
    0x0F: (3, "Xebec WriteSectorBuffer"),
    0x10: (3, "WriteFilemarks"),
    0x11: (1, "Space"),
    0x12: (0, "Inquiry"),
    0x13: (1, "Verify"),
    0x15: (0, "ModeSelect6"),
    0x16: (2, "Reserve"),
    0x17: (2, "Release"),
    0x19: (2, "Erase"),
    0x1A: (0, "ModeSense"),
    0x1B: (0, "StartStopUnit"),
    0x1C: (0, "ReceiveDiagnostic"),
    0x1D: (0, "SendDiagnostic"),
    0x1E: (0, "PreventAllowMediumRemoval"),
    0x25: (0, "ReadCapacity"),
    0x28: (1, "Read10"),
    0x2A: (2, "Write10"),
    0x2B: (1, "Seek10"),
    0x2C: (2, "Erase10"),
    0x2E: (2, "WriteVerify"),
    0x2F: (1, "Verify"),
    0x34: (1, "PreFetch/ReadPosition"),
    0x35: (0, "SynchronizeCache"),
    0x36: (0, "LockUnlockCache"),
    0x37: (0, "ReadDefectData"),
    0x3B: (2, "WriteBuffer"),
    0x3C: (1, "ReadBuffer"),
    0x42: (1, "CDROM Read SubChannel"),
    0x43: (1, "CDROM Read TOC"),
    0x44: (1, "CDROM Read Header"),
    0x46: (0, "CDROM GetConfiguration"),
    0x4A: (0, "GetEventStatusNotification"),
    0x4B: (0, "CDROM PauseResume"),
    0x4E: (0, "CDROM StopPlayScan"),
    0x51: (1, "CDROM ReadDiscInformation"),
    0x45: (1, "CDROM PlayAudio10"),
    0xA5: (1, "CDROM PlayAudio12"),
    0x47: (1, "CDROM PlayAudioMSF"),
    0x48: (1, "CDROM PlayAudioTrackIndex"),
    0x52: (1, "CDROM ReadTrackInformation"),
    0x88: (1, "Read16"),
    0x8A: (2, "Write16"),
    0x8E: (2, "WriteVerify16"),
    0x8F: (1, "Verify16"),
    0x9E: (3, "ServiceActionIn16"),
    0xBB: (0, "CDROM SetCDSpeed"),
    0xBD: (0, "CDROM MechanismStatus"),
    0xBE: (1, "ReadCD"),
    0xB9: (1, "ReadCDMSF"),
    0x55: (0, "ModeSelect10"),
    0x5A: (0, "ModeSense10"),
    0xA8: (1, "Read12"),
    0xAA: (2, "Write12"),
    0xAC: (2, "Erase12"),
    0xAE: (2, "WriteVerify12"),
    0xAF: (1, "Verify12"),
    0xC0: (3, "OMTI-5204 DefineFlexibleDiskFormat"),
    0xC2: (3, "OMTI-5204 AssignDiskParameters"),
    0xD0: (3, "Vendor 0xD0 Command (Toolbox list files)"),
    0xD1: (3, "Vendor 0xD1 Command (Toolbox get file)"),
    0xD2: (3, "Vendor 0xD2 Command (Toolbox count files)"),
    0xD3: (3, "Vendor 0xD3 Command (Toolbox send file prep)"),
    0xD4: (3, "Vendor 0xD4 Command (Toolbox send file 10)"),
    0xD5: (3, "Vendor 0xD5 Command (Toolbox send file end)"),
    0xD6: (3, "Vendor 0xD6 Command (Toolbox toggle debug)"),
    0xD7: (3, "Vendor 0xD7 Command (Toolbox list CDs)"),
    0xD8: (3, "Vendor 0xD8 Command (Toolbox set next CD/Apple/Plextor)"),
    0xD9: (3, "Vendor 0xD9 Command (Toolbox list devices/Apple)"),
    0xDA: (3, "Vendor 0xDA Command (Toolbox count CDs)"),
    0xE0: (3, "Xebec RAM Diagnostic"),
    0xE4: (3, "Xebec Drive Diagnostic"),
}

class Pins:
    '''Indexes must match optional_channels tuple'''
    BSY = 0
    ACK = 1
    REQ = 2
    MSG = 3
    CD = 4
    SEL = 5
    RST = 6
    IO = 7
    ATN = 8
    DATA_DIR = 9
    DBP = 10
    DBP1 = 11
    D0 = 12
    D8 = 12 + 8
    D16 = 12 + 16
    
class Annotations:
    '''Indexes must match annotation classes'''
    state = 0
    datain = 1
    dataout = 2
    cmd_ctrl = 3
    cmd_rd = 4
    cmd_wr = 5
    cmd_special = 6

class SCSIStates:
    BUS_FREE = -1
    BUS_BUSY = -2
    ARBITRATION = -3
    SELECTION = -4
    STATUS = 6
    COMMAND = 2
    DATA_IN = 4
    DATA_OUT = 0
    MESSAGE_IN = 7
    MESSAGE_OUT = 3
    BIT_IO = 4
    BIT_CD = 2
    BIT_MSG = 1

    def from_pins(pins):
        s = 0
        if pins[Pins.MSG] == 0: s |= SCSIStates.BIT_MSG
        if pins[Pins.CD] == 0: s |= SCSIStates.BIT_CD
        if pins[Pins.IO] == 0: s |= SCSIStates.BIT_IO
        return s

    def to_str(state):
        if state == SCSIStates.BUS_FREE:       return "BUS_FREE"
        if state == SCSIStates.BUS_BUSY:       return "BUS_BUSY"
        if state == SCSIStates.ARBITRATION:    return "ARBITRATION"
        if state == SCSIStates.SELECTION:      return "SELECTION"
        if state == SCSIStates.STATUS:         return "STATUS"
        if state == SCSIStates.COMMAND:        return "COMMAND"
        if state == SCSIStates.DATA_IN:        return "DATA_IN"
        if state == SCSIStates.DATA_OUT:       return "DATA_OUT"
        if state == SCSIStates.MESSAGE_IN:     return "MESSAGE_IN"
        if state == SCSIStates.MESSAGE_OUT:    return "MESSAGE_OUT"
        return "UNKNOWN STATE"
    
    def to_str_short(state):
        if state == SCSIStates.BUS_FREE:       return "FREE"
        if state == SCSIStates.BUS_BUSY:       return "BUSY"
        if state == SCSIStates.ARBITRATION:    return "ARB."
        if state == SCSIStates.SELECTION:      return "SEL"
        if state == SCSIStates.STATUS:         return "STAT"
        if state == SCSIStates.COMMAND:        return "CMD"
        if state == SCSIStates.DATA_IN:        return "D_IN"
        if state == SCSIStates.DATA_OUT:       return "D_OUT"
        if state == SCSIStates.MESSAGE_IN:     return "M_IN"
        if state == SCSIStates.MESSAGE_OUT:    return "M_OUT"
        return "?"

class Decoder(srd.Decoder):
    api_version = 3
    id = 'scsi'
    name = 'SCSI'
    longname = 'SCSI bus'
    desc = 'SCSI bus'
    license = 'gplv3+'
    inputs = ['logic']
    outputs = []
    tags = ['Util']
    optional_channels = tuple([
        {'id': 'bsy', 'name': 'BSY', 'desc': 'Busy'},
        {'id': 'ack', 'name': 'ACK', 'desc': 'Acknowledge from host'},
        {'id': 'req', 'name': 'REQ', 'desc': 'Request to host'},
        {'id': 'msg', 'name': 'MSG', 'desc': 'Message state'},
        {'id': 'cd',  'name': 'CD',  'desc': 'Command state'},
        {'id': 'sel', 'name': 'SEL', 'desc': 'Select device'},
        {'id': 'rst', 'name': 'RST', 'desc': 'Reset'},
        {'id': 'io',  'name': 'IO', 'desc': 'Data direction'},
        {'id': 'atn', 'name': 'ATN', 'desc': 'Attention'},
        {'id': 'data_dir', 'name': 'DATA_DIR', 'desc': 'Data direction (ZuluSCSI)'},
        {'id': 'dbp', 'name': 'DBP', 'desc': 'Data parity'},
        {'id': 'dbp1', 'name': 'DBP1', 'desc': 'Data parity high'},
        ] + [{'id': 'd%d' % i, 'name': 'DB%d' % i, 'desc': 'Data line %d' % i} for i in range(16)]
    )
    options = (
        {'id': 'invert_data', 'desc': 'Invert polarity of data bytes',
         'default': 'auto', 'values': ('auto', 'on', 'off')},
    )
    annotations = (
        ('state', 'Protocol state'),
        ('datain', 'Data transfers to initiator'),
        ('dataout', 'Data transfers to target'),
        ('cmd_ctrl', 'Control commands'),
        ('cmd_rd', 'Read commands'),
        ('cmd_wr', 'Write commands'),
        ('cmd_special', 'Special commands'),
    )
    annotation_rows = (
        ('state_row', 'Current state', (Annotations.state,)),
        ('commands', 'Commands', (Annotations.cmd_ctrl, Annotations.cmd_rd, Annotations.cmd_wr, Annotations.cmd_special)),
        ('data_row', 'Data transfers', (Annotations.datain, Annotations.dataout)),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.cmd_bytes = []
        self.state = SCSIStates.BUS_FREE
        self.ss_state = None
        self.next_ss_state = None
        self.pending_state = None
        self.ss_data = None
        self.ss_cmd = None
        self.is_wide = False
        self.pending_data = None
        self.prev_req = None
        self.prev_ack = None
        self.phase_data = []
        self.invert_data = None
        
    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)

    def push_cmd(self, end_sample):
        if not self.cmd_bytes:
            self.put(self.ss_cmd, end_sample, self.out_ann, [Annotations.cmd, ["NO COMMAND"]])
        else:
            cmdclass, cmdname = SCSI_COMMANDS.get(self.cmd_bytes[0], (3,"UNKNOWN COMMAND"))
            texts = [cmdname + ": " + ' '.join('%02x' % x for x in self.cmd_bytes), cmdname]
            self.put(self.ss_cmd, end_sample, self.out_ann, [Annotations.cmd_ctrl + cmdclass, texts])
        
        self.cmd_bytes = []

    def push_state(self, end_sample):
        unit = 'byte'
        if self.is_wide and self.state in (SCSIStates.DATA_IN, SCSIStates.DATA_OUT): unit = 'word'
        if len(self.phase_data) != 1: unit += 's'

        texts = [
            SCSIStates.to_str(self.state) + " (%d %s: %s)" % (len(self.phase_data), unit, ' '.join('%02x' % x for x in self.phase_data[:8])),
            SCSIStates.to_str(self.state) + " (%d %s)" % (len(self.phase_data), unit),
            SCSIStates.to_str(self.state),
            SCSIStates.to_str_short(self.state)]
    
        if len(self.phase_data) == 0: texts = texts[-2:]
        self.put(self.ss_state, end_sample, self.out_ann, [Annotations.state, texts])

    def push_data(self):
        if self.pending_data is not None:
            if self.state in (SCSIStates.DATA_IN, SCSIStates.DATA_OUT) and self.is_wide:
                texts = ["0x%04x" % self.pending_data, "%04x" % self.pending_data, "%02x" % (self.pending_data & 0xFF)]
            else:
                texts = ["0x%02x" % (self.pending_data & 0xFF), "%02x" % (self.pending_data & 0xFF)]

            dir = (Annotations.datain if (self.state & SCSIStates.BIT_IO) else Annotations.dataout)

            self.put(self.ss_data, self.samplenum, self.out_ann, [dir, texts])

            if self.state == SCSIStates.COMMAND:
                self.cmd_bytes.append(self.pending_data & 0xFF)

    def process_eof_states(self):
        '''Dump out the end of any current state'''
        if self.state != SCSIStates.BUS_FREE:
            self.push_state(self.samplenum)
            self.push_cmd(self.samplenum)
            self.push_data()

    def decode(self):
        wait_conditions = [{Pins.BSY: 'e'}, {Pins.REQ: 'e'}, {Pins.ACK: 'e'},
                           {Pins.MSG: 'e'}, {Pins.CD: 'e'}, {Pins.IO: 'e'},
                           {Pins.DATA_DIR: 'e'}]
        while True:
            try:
                pins = self.wait(wait_conditions)
            except EOFError:
                self.process_eof_states()
                break

            if self.invert_data is None:
                if self.options['invert_data'] == 'auto':
                    if pins[Pins.BSY]:
                        if bitpack(pins[Pins.D0:Pins.D8]) == 0x00:
                            self.invert_data = False
                        elif bitpack(pins[Pins.D0:Pins.D8]) == 0xFF:
                            self.invert_data = True
                elif self.options['invert_data'] == 'on':
                    self.invert_data = True
                else:
                    self.invert_data = False

            if not pins[Pins.BSY] and self.state == SCSIStates.BUS_FREE:
                # Start of transaction
                self.state = SCSIStates.BUS_BUSY
                self.ss_state = self.samplenum
                self.ss_cmd = self.samplenum
                self.cmd_bytes = []
                self.phase_data = []
                self.pending_data = None    
            elif pins[Pins.BSY] and self.state != SCSIStates.BUS_FREE:
                # End of transaction
                self.push_cmd(self.samplenum)
                self.push_state(self.samplenum)
                self.state = SCSIStates.BUS_FREE
            
            if 0 in pins[Pins.D8:Pins.D16]:
                self.is_wide = True
            
            # Monitor when state pins change, but they take effect only on REQ
            new_state = SCSIStates.from_pins(pins)
            if new_state != self.pending_state and pins[Pins.REQ] == 1:
                self.pending_state = new_state
                self.next_ss_state = self.samplenum
                self.pending_data = None

            if pins[Pins.REQ] == 0 and self.prev_req == 1:
                if pins[Pins.DATA_DIR] == 1 and pins[Pins.IO] not in [0, 1]:
                    # Synthetize SCSI_IO state bit for ZuluSCSI Wide where it doesn't fit the capture
                    new_state |= SCSIStates.BIT_IO
                    self.next_ss_state = self.samplenum

                # State changes get applied when REQ goes low
                if new_state != self.state:
                    self.push_state(self.next_ss_state)
                    self.state = new_state
                    self.ss_state = self.next_ss_state
                    self.next_ss_state = self.samplenum
                    self.phase_data = []
                    self.pending_data = None
            
            # Data to initiator is latched on REQ falling edge, to target on ACK falling edge
            if self.state & SCSIStates.BIT_IO:
                latch_data = (pins[Pins.REQ] == 0 and self.prev_req == 1)
            else:
                latch_data = (pins[Pins.ACK] == 0 and self.prev_ack == 1)
            
            # If the sniffer is too slow, we can sometimes miss either ACK or REQ pulse
            # Fill it in with a guess
            if not (self.state & SCSIStates.BIT_IO):
                if pins[Pins.REQ] == 1 and self.prev_req == 0 and pins[Pins.ACK] == 1 and self.prev_ack == 1:
                    latch_data = True

            if latch_data:
                if self.is_wide:
                    self.pending_data = bitpack(pins[Pins.D0:Pins.D16])
                    if self.invert_data: self.pending_data ^= 0xFFFF
                else:
                    self.pending_data = bitpack(pins[Pins.D0:Pins.D8])
                    if self.invert_data: self.pending_data ^= 0xFF
                self.ss_data = self.samplenum

            if self.pending_data is not None:
                if not latch_data or (pins[Pins.REQ] == 1 and pins[Pins.ACK] == 1):
                    if not self.ss_data or self.ss_data >= self.samplenum:
                        self.ss_data = self.samplenum - 1
                    
                    self.phase_data.append(self.pending_data)
                    self.push_data()
                    self.pending_data = None
            
            self.prev_req = pins[Pins.REQ]
            self.prev_ack = pins[Pins.ACK]



