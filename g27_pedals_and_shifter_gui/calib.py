import struct
import ctypes as ct
import sys
import time
import os
import traceback
from PySide2.QtCore import QRectF, Qt, QObject, QThread, Signal, QMutex, QMutexLocker, QTimer, QSignalBlocker
from PySide2.QtGui import QBrush, QPen, QColor
from PySide2.QtWidgets import (QApplication, QWidget, QGroupBox, QLabel, QGridLayout, QVBoxLayout, QRadioButton,
                               QCheckBox, QComboBox, QPushButton)
import serial
from serial.tools import list_ports
import pyqtgraph as pg
import numpy as np
# all joystick inputs seem to have issues with the more than 16 buttons present on G27 shifter.
# therefore, we just use inputs to connect to the device and all other stuff is transferred via serial
# port.
import inputs

FLAG1_INVERT_BRAKE = 0x1
FLAG1_INVERT_GAS = 0x2
FLAG1_INVERT_CLUTCH = 0x4
FLAG1_REVERSE_RIGHT_RED = 0x8
FLAG1_COMBINE_Z_EQUALS_X_MINUS_Z = 0x10
FLAG1_COMBINE_Z_EQUALS_Z_MINUS_X = 0x20
FLAG2_ENABLE_PEDALS = 0x1
FLAG2_BRAKE_DEAD_ZONE = (0x2|0x4|0x8)
FLAG2_GAS_DEAD_ZONE = (0x10|0x20|0x40)
FLAG3_ENABLE_SHIFTER = 0x1
FLAG3_CLUTCH_DEAD_ZONE = (0x10|0x20|0x40)

# following structures need to be synchronized with Arduino C Code
class CalibData(ct.Structure):
    _pack_ = 2
    _fields_ = [
        ("calibID", ct.c_uint32),
        ("pedals_auto_calib", ct.c_uint8),
        ("flag1", ct.c_uint8),
        ("flag2", ct.c_uint8),
        ("flag3", ct.c_uint8),
        ("pedals_median_size", ct.c_uint8),
        ("shifter_median_size", ct.c_uint8),
        ("gasMin", ct.c_uint16),
        ("gasMax", ct.c_uint16),
        ("brakeMin", ct.c_uint16),
        ("brakeMax", ct.c_uint16),
        ("clutchMin", ct.c_uint16),
        ("clutchMax", ct.c_uint16),
        ("shifter_y_neutralMin", ct.c_uint16),
        ("shifter_y_neutralMax", ct.c_uint16),
        ("shifter_y_246R_gearZone", ct.c_uint16),
        ("shifter_y_135_gearZone", ct.c_uint16),
        ("shifter_x_12", ct.c_uint16),
        ("shifter_x_56", ct.c_uint16),
    ]

class DebugStruct(ct.Structure):
    _pack_ = 2
    _fields_ = [
        ("sizeInBytes", ct.c_uint8),
        ("calibButton", ct.c_uint8),
        ("axisValues", ct.c_uint16*5),
        ("calib", CalibData),
        ("numCrcErrors", ct.c_uint16),
        ("numMagicNumErrors", ct.c_uint16),
        ("profiling", ct.c_uint32*4),
        ("out_axis", ct.c_uint16*3),
        ("out_buttons", ct.c_uint32)
    ]


class Values:
    def __init__(self, history_time_s = 1.0):
        self.mutex = QMutex()
        self.history_time_ns = np.int64(history_time_s*1e9)
        self.times = np.zeros((0,), dtype=np.int64)
        self.gas = np.zeros((0,), dtype=np.uint16)
        self.brake = np.zeros((0,), dtype=np.uint16)
        self.clutch = np.zeros((0,), dtype=np.uint16)
        self.sx = np.zeros((0,), dtype=np.uint16)
        self.sy = np.zeros((0,), dtype=np.uint16)
        self.calib = CalibData()

    def update(self, dbg): # options
        ml = QMutexLocker(self.mutex)
        self.times = np.append(self.times, [time.perf_counter_ns()])
        v = self.times >= self.times[-1] - self.history_time_ns
        self.times = self.times[v]
        self.gas = np.append(self.gas, [dbg.axisValues[2]])[v]
        self.brake = np.append(self.brake, [dbg.axisValues[3]])[v]
        self.clutch = np.append(self.clutch, [dbg.axisValues[4]])[v]
        self.sx = np.append(self.sx, [dbg.axisValues[0]])[v]
        self.sy = np.append(self.sy, [dbg.axisValues[1]])[v]
        self.calib = dbg.calib

    def add(self, other):
        ml = QMutexLocker(other.mutex)
        ml2 = QMutexLocker(self.mutex)
        self.times = np.append(self.times, other.times)
        self.gas = np.append(self.gas, other.gas)
        self.brake = np.append(self.brake, other.brake)
        self.clutch = np.append(self.clutch, other.clutch)
        self.sx = np.append(self.sx, other.sx)
        self.sy = np.append(self.sy, other.sy)
        self.calib = other.calib
        other.times = np.resize(other.times, (0,))
        other.gas = np.resize(other.gas, (0,))
        other.brake = np.resize(other.brake, (0,))
        other.clutch = np.resize(other.clutch, (0,))
        other.sx = np.resize(other.sx, (0,))
        other.sy = np.resize(other.sy, (0,))
        v = self.times >= self.times[-1] - self.history_time_ns
        self.times = self.times[v]
        self.gas = self.gas[v]
        self.brake = self.brake[v]
        self.clutch = self.clutch[v]
        self.sx = self.sx[v]
        self.sy = self.sy[v]

class G27CalibGui(QWidget):
    sendModeCmd = Signal(object)
    setObjectCmd = Signal(object)

    def __init__(self):
        super().__init__()
        self.tl_layout = QGridLayout(self)
        rbtn_values = QGroupBox("Calibration Values", self)
        vbox = QVBoxLayout(rbtn_values)
        self.mode_btns = [
            QRadioButton(s, rbtn_values) for s in
                    ["Idle", "Shifter neutral zone", "Shifter 135 zone", "Shifter 246R zone", "Shifter 12 zone",
                    "Shifter 56 zone", "Gas pedal (if not auto-calibrated)", "Brake pedal (if not auto-calibrated)",
                    "Clutch pedal (if not auto-calibrated)"]]
        self.mode_btns[0].setChecked(True)
        self.mode_cmds = [b"i", b"n", b"u", b"b", b"l", b"r", b"G", b"B", b"C"]
        self.help_cmds = [
            "First decide about the filtering needed (in middle panel). If there is a lot of noise in the signal, "
                "maybe because of old pots, you might want to use a multi-sample median filter. Note that this filter "
                "introduces some delay, so you want to keep the numbers reasonable low. Afterwards, iterate through "
                "the calibration values in the left panel.\n\n"
                "When you're done, you can save the calibration in the right panel to the EEPROM.",
            "Put the shifter in neutral position. Afterwards, keep the left red button pressed and move the "
                "shifter while still in neutral. When reaching the calibrated area, the gear will be set to neutral.",
            "Put the shifter in 3rd gear. Afterwards keep the left red button pressed and move the shifter "
                "while still in 3rd gear. When done, release the red button. Optionally repeat for gears 1st and 5th.",
            "Put the shifter in 4th gear. Afterwards keep the left red button pressed and move the shifter "
                "while still in 4th gear. When done, release the red button. Optionally repeat for gears 2nd and 6th.",
            "Put the shifter in 1st gear. Afterwards keep the left red button pressed and move the shifter "
                "while still in 1st gear. When done, release the red button. Optionally repeat for 2nd gear.",
            "Put the shifter in 5th gear. Afterwards keep the left red button pressed and move the shifter "
                "while still in 5th gear. When done, release the red button. Optionally repeat for 6th gear.",
            "Move the gas pedal multiple times between lowest and highest position.",
            "Move the brake pedal multiple times between lowest and highest position.",
            "Move the clutch pedal multiple times between lowest and highest position."
        ]
        for b in self.mode_btns:
            vbox.addWidget(b)
            b.toggled.connect(self.modeChanged)
        self.tl_layout.addWidget(rbtn_values, 0,0, 1, 1)

        cbtn_options = QGroupBox("Options", self)
        grid = QGridLayout(cbtn_options)
        self.option_btns = [
            QCheckBox(s, cbtn_options) for s in
                ["Enable pedal auto-calibration",
                 "Invert gas/throttle pedal",
                 "Invert brake pedal",
                 "Invert clutch pedal",
                 "Use right red button for reverse instead pushing the gear",
                 "Enable pedals",
                 "Enable shifter"]
        ] + [QComboBox(), QComboBox(), QComboBox(), QComboBox(), QComboBox(), QComboBox()]
        self.option_cmds = [(b"p", b"P"), #0
                            (b"y", b"Y"), #1
                            (b"x", b"X"), #2
                            (b"z", b"Z"), #3
                            (b"q", b"Q"), #4
                            (b"e", b"E"), #5
                            (b"s", b"S"), #6
                            (b".", b"+", b"-"), #7
                            (b"[^", b"[!", b"[\"", b"[]", b"[$", b"[%", b"[&", b"[/"),
                            (b"(^", b"(!", b"(\"", b"(]", b"($", b"(%", b"(&", b"(/"),
                            (b")^", b")!", b")\"", b")]", b")$", b")%", b")&", b")/"),                            
                            (b"0", b"3", b"5", b"7", b"9", b"f"),
                            (b"1", b"2", b"4", b"6", b"8", b"F"),
                            ]
        self.option_btns[-6].addItem("Do not combine clutch and gas")
        self.option_btns[-6].addItem("Combine Clutch and Gas (Clutch = Gas - Clutch)")
        self.option_btns[-6].addItem("Combine Clutch and Gas (Clutch = Clutch - Gas)")
        for (idx, name) in [(-5, "Gas"), (-4, "Brake"), (-3, "Clutch")]:
            for dz in [0,2,4,6,8,10,12,14]:
                self.option_btns[idx].addItem("%s: %d%% dead zone" % (name, dz))
        for (idx, name) in [(-2, "pedals"), (-1, "shifter")]:
            for size in [0,3,5,9,15,49]:
                self.option_btns[idx].addItem("off" if size == 0 else (str(size) + "-median"), size)
        for i,b in enumerate(self.option_btns):
            n_checkboxes = len(self.option_cmds) - 6
            if i < n_checkboxes:
                grid.addWidget(b, i, 0, 1, 2)
            else:
                names = {n_checkboxes: "Combine", 
                         n_checkboxes + 1: "Gas Dead Zone", n_checkboxes + 2: "Brake Dead Zone", n_checkboxes + 3: "Clutch Dead Zone",
                         n_checkboxes + 4: "Pedal filter", n_checkboxes + 5: "Shifter filter"}
                grid.addWidget(QLabel(names[i], parent=self), i, 0)
                grid.addWidget(b, i, 1)
            if isinstance(b, QCheckBox):
                b.toggled.connect(self.optionChanged)
            else:
                b.currentIndexChanged.connect(self.optionChanged)
        self.tl_layout.addWidget(cbtn_options, 0, 1, 1, 1)

        cbtn_persistent = QGroupBox("Persistency")
        self.persistent_btns = [QPushButton(s, parent=self) for s in ["Save to EEPROM", "Load from EEPROM", "Defaults"]]
        self.persistent_cmds = [b"w", b"U", b"c"]
        vbox = QVBoxLayout(cbtn_persistent)
        for i,b in enumerate(self.persistent_btns):
            vbox.addWidget(b)
            b.clicked.connect(self.persistent_cmd)
        self.tl_layout.addWidget(cbtn_persistent, 0, 2, 1, 1)

        self.helpArea = QLabel(self.help_cmds[0], self)
        self.helpArea.setWordWrap(True)
        f = self.helpArea.font()
        f.setBold(True)
        f.setPointSizeF(f.pointSizeF()*1.5)
        self.helpArea.setFont(f)
        self.tl_layout.addWidget(self.helpArea, 0, 3, 1, 1)

        shifter_label = QLabel("Shifter")
        self.tl_layout.addWidget(shifter_label, 1,2,1,2,alignment=Qt.AlignHCenter)
        shifter_plot = pg.PlotWidget(parent=self)
        shifter_plot.setMouseEnabled(x=False,y=False)
        self.tl_layout.addWidget(shifter_plot, 2,2, 1,2)
        shifter_plot.setRange(QRectF(0,0,1023,1023), padding=0)
        pi = shifter_plot.getPlotItem()
        self.shifter_neutral_plot = pg.LinearRegionItem(values = (400, 600), orientation='horizontal', movable=False,
                                                        brush = QBrush(QColor(200,50,50,50)),
                                                        pen=QPen(QColor(200,50,50)))
        shifter_plot.addItem(self.shifter_neutral_plot)
        self.shifter_135_plot = pg.LinearRegionItem(values = (800, 1024), orientation='horizontal', movable=False,
                                                    brush = QBrush(QColor(50,200,50,50)),
                                                    pen = QPen(QColor(50,200,50)))
        shifter_plot.addItem(self.shifter_135_plot)
        self.shifter_246_plot = pg.LinearRegionItem(values = (0, 200), orientation='horizontal', movable=False,
                                                    brush = QBrush(QColor(50,50,200,50)),
                                                    pen = QPen(QColor(50,50,200)))
        shifter_plot.addItem(self.shifter_246_plot)
        self.shifter_12_plot = pg.LinearRegionItem(values = (0, 200), orientation='vertical', movable=False,
                                                    brush = QBrush(QColor(50,200,200,50)),
                                                    pen = QPen(QColor(50,200,200)))
        shifter_plot.addItem(self.shifter_12_plot)
        self.shifter_56_plot = pg.LinearRegionItem(values = (800, 1024), orientation='vertical', movable=False,
                                                    brush = QBrush(QColor(200,200,50,50)),
                                                    pen = QPen(QColor(200,200,50)))
        shifter_plot.addItem(self.shifter_56_plot)
        self.shifter_pos_plot = pi.plot(x=[512], y=[512], symbol='+', pen=None)

        pedals_label = QLabel("Pedals")
        self.tl_layout.addWidget(pedals_label, 1,0,1,2,alignment=Qt.AlignHCenter)
        pedals_plot = pg.PlotWidget(parent=self)
        pedals_plot.setMouseEnabled(x=False,y=False)
        pedals_plot.setRange(QRectF(-1,0,4,1023), padding=0)
        pedals_plot.getAxis('bottom').setTicks([[(0.0,"Gas/Throttle"), (1.0,"Brake"), (2.0,"Clutch")],[]])
        self.tl_layout.addWidget(pedals_plot,2,0,1,2)
        self.pedals_pos_plot = pg.BarGraphItem(x=np.arange(3), height=[512,512,512], width=0.5)
        self.pedals_gas_calib_plot = pedals_plot.getPlotItem().plot(x=[0], y=[0], pen=QPen(QColor(255,0,0),width=0.1), brush=QBrush(Qt.NoBrush))
        self.pedals_brake_calib_plot = pedals_plot.getPlotItem().plot(x=[0], y=[0], pen=QPen(QColor(0,255,0)))
        self.pedals_clutch_calib_plot = pedals_plot.getPlotItem().plot(x=[0], y=[0], pen=QPen(QColor(0,0,255)))
        self.pedals_pos_plot2 = pedals_plot.getPlotItem().plot(x=[], y=[], symbol='+', pen=None)
        pedals_plot.addItem(self.pedals_pos_plot)

        self.status_line = QLabel(self)
        self.tl_layout.addWidget(self.status_line,3,0,1,2)
        self.status_line2 = QLabel(self)
        self.tl_layout.addWidget(self.status_line2,3,2,1,2)
        self.currentVals = Values()

    def modeChanged(self, toggled):
        if toggled:
            o = self.sender()
            idx = self.mode_btns.index(o)
            self.sendModeCmd.emit(self.mode_cmds[idx])
            self.helpArea.setText(self.help_cmds[idx])

    def optionChanged(self, option):
        o = self.sender()
        idx = self.option_btns.index(o)
        self.sendModeCmd.emit(self.option_cmds[idx][int(option)])

    def persistent_cmd(self):
        o = self.sender()
        idx = self.persistent_btns.index(o)
        self.sendModeCmd.emit(self.persistent_cmds[idx])

    def newVals(self, values, dbg):
        updateRate = 0.040*1e9 # 25 Hz
        if len(self.currentVals.times) == 0 or (len(values.times) > 0 and
                                                values.times[-1] > self.currentVals.times[-1] + updateRate): # each 10ms
            self.currentVals.add(values)
            values = self.currentVals
            self.pedals_pos_plot.setOpts(height=[values.gas[-1], values.brake[-1], values.clutch[-1]])
            self.pedals_pos_plot2.setData(x=np.concatenate(([-0.35]*len(values.gas), [0.65]*len(values.brake), [1.65]*len(values.clutch))),
                                          y=np.concatenate((values.gas, values.brake, values.clutch)))

            self.pedals_gas_calib_plot.setPen(QPen(QColor(255,0,0),width=0))
            self.pedals_gas_calib_plot.setData(x=[-0.0, -0.0, +0.0, +0.0, -0.0],
                                               y=[values.calib.gasMin, values.calib.gasMax, values.calib.gasMax, values.calib.gasMin, values.calib.gasMin])
            self.pedals_brake_calib_plot.setData(x=[1.0,1.0,1.0,1.0,1.0],
                                                 y=[values.calib.brakeMin, values.calib.brakeMax, values.calib.brakeMax, values.calib.brakeMin, values.calib.brakeMin])
            self.pedals_clutch_calib_plot.setData(x=[2.0,2.0,2.0,2.0,2.0],
                                                  y=[values.calib.clutchMin, values.calib.clutchMax, values.calib.clutchMax, values.calib.clutchMin, values.calib.clutchMin])

            self.shifter_pos_plot.setData(x=values.sx, y=values.sy)
            self.shifter_neutral_plot.setRegion((values.calib.shifter_y_neutralMin, values.calib.shifter_y_neutralMax))
            self.shifter_135_plot.setRegion((values.calib.shifter_y_135_gearZone, 1024))
            self.shifter_246_plot.setRegion((0, values.calib.shifter_y_246R_gearZone))
            self.shifter_12_plot.setRegion((0, values.calib.shifter_x_12))
            self.shifter_56_plot.setRegion((values.calib.shifter_x_56, 1024))

            blockers = []
            for b in self.option_btns:
                blockers.append(QSignalBlocker(b))
            self.option_btns[0].setChecked(values.calib.pedals_auto_calib)
            self.option_btns[1].setChecked((values.calib.flag1 & FLAG1_INVERT_GAS) != 0)
            self.option_btns[2].setChecked((values.calib.flag1 & FLAG1_INVERT_BRAKE) != 0)
            self.option_btns[3].setChecked((values.calib.flag1 & FLAG1_INVERT_CLUTCH) != 0)
            self.option_btns[4].setChecked((values.calib.flag1 & FLAG1_REVERSE_RIGHT_RED) != 0)
            self.option_btns[5].setChecked((values.calib.flag2 & FLAG2_ENABLE_PEDALS) != 0)
            self.option_btns[6].setChecked((values.calib.flag3 & FLAG3_ENABLE_SHIFTER) != 0)
            if (values.calib.flag1 & (FLAG1_COMBINE_Z_EQUALS_X_MINUS_Z | FLAG1_COMBINE_Z_EQUALS_Z_MINUS_X)) == 0:
                self.option_btns[7].setCurrentIndex(0)
            elif (values.calib.flag1 & FLAG1_COMBINE_Z_EQUALS_X_MINUS_Z) != 0:
                self.option_btns[7].setCurrentIndex(1)
            else:
                self.option_btns[7].setCurrentIndex(2)
            self.option_btns[8].setCurrentIndex((values.calib.flag2 & FLAG2_GAS_DEAD_ZONE) >> 4)
            self.option_btns[9].setCurrentIndex((values.calib.flag2 & FLAG2_BRAKE_DEAD_ZONE) >> 1)
            self.option_btns[10].setCurrentIndex((values.calib.flag3 & FLAG3_CLUTCH_DEAD_ZONE) >> 4)
            self.option_btns[11].setCurrentIndex(self.option_btns[11].findData(values.calib.pedals_median_size))
            self.option_btns[12].setCurrentIndex(self.option_btns[12].findData(values.calib.shifter_median_size))

            prof = "Total runtime: %9.2f ms | prof[0->1]: %9.2f ms | prof[1->2]: %9.2f ms | prof[2->3]: %9.2f ms | FPS: %04d" % (
                    (dbg.profiling[-1] - dbg.profiling[0])*1e-3,
                    (dbg.profiling[1] - dbg.profiling[0])*1e-3,
                    (dbg.profiling[2] - dbg.profiling[1])*1e-3,
                    (dbg.profiling[3] - dbg.profiling[2])*1e-3,
                    ((len(values.times)*1e9 / (values.times[-1] - values.times[0])) if len(values.times) > 1 else "N/A"),
            )
            self.status_line.setText(prof)

            # output values
            s = ""
            if dbg.out_axis[0] != 0xffff:
                s += "A0=%04d A1=%04d A2=%04d " % tuple(dbg.out_axis)
            else:
                s += "A0=N/A  A1=N/A  A2=N/A  "

            if dbg.out_buttons != 0xffffffff:
                bstr="{:019b}".format(dbg.out_buttons)
                s += "ShifterBtn: " + bstr[:-7]
                gear = bstr[-7:][::-1]
                if "1" in gear:
                    gear = gear.index("1") + 1
                else:
                    gear = 0
                s += " ShifterGear=%d" % (gear)
            else:
                s += "Shifter=N/A"
            self.status_line2.setText(s)

class Collector(QObject):

    valuesChanged = Signal(object, object)

    def __init__(self, portName):
        super().__init__()
        self.portName = portName
        self.thread = QThread()
        self.moveToThread(self.thread)
        self.thread.started.connect(self.create)
        self.thread.start()

    def sendModeCmd(self, cmd):
        self.serialPort.write(cmd)
        #print("Sent CMD: ", cmd)
        
    def stop(self):
        print("Disabling serial monitor.")
        self.serialPort.write(b'o')
        self.timer.stop()
        self.thread.quit()

    def create(self):
        try:
            self.values = Values()
            self.serialPort = serial.Serial(self.portName, baudrate=460800, timeout=1) #115200, timeout=1)
            self.serialPort.write(b'O')
            self.n = 0
            self.tstart = time.perf_counter_ns()
            self.timer = QTimer()
            self.timer.timeout.connect(self.readFromSerial, Qt.QueuedConnection)
            self.timer.setInterval(0)
            self.timer.start()
        except Exception as e:
            traceback.print_exc()

    def readFromSerial(self):
        try:
            buf = self.serialPort.read(1)
            numBytes, = struct.unpack("B", buf)
            while len(buf) < numBytes-1:
                buf += self.serialPort.read(numBytes-1)
            dbg = DebugStruct.from_buffer_copy(buf)
            self.n += len(buf)+2
            if 0:
                print("read %d bytes in %.2f seconds: bits/second=%.1f" %
                      (self.n,
                       (time.perf_counter_ns() - self.tstart)*1e-9,
                       self.n * 8 / ((time.perf_counter_ns() - self.tstart)*1e-9)))
            if 0:
                print("Profiling 01: %.1f ms   12: %.1f ms   23: %.1f ms      03: %.1f ms" %
                      (
                          (items[-3] - items[-4])*1e-3,
                          (items[-2] - items[-3])*1e-3,
                          (items[-1] - items[-2])*1e-3,
                          (items[-1] - items[-4])*1e-3,
                      ))
            self.values.update(dbg)
            self.valuesChanged.emit(self.values, dbg)
        except Exception as e:
            traceback.print_exc()

class JoystickSink(QObject):
    def __init__(self, jsdev):
        super().__init__()
        self.jsdev = jsdev
        self.thread = QThread()
        self.moveToThread(self.thread)
        self.thread.started.connect(self.create)
        self.thread.start()

    def create(self):
        self.timer = QTimer()
        self.timer.timeout.connect(self.readFromDevice, Qt.QueuedConnection)
        self.timer.setInterval(0)
        self.timer.start()
        
    def stop(self):
        self.timer.stop()
        self.thread.quit()

    def readFromDevice(self):
        try:
            events = self.jsdev.read()
        except:
            pass


def main():
    app = QApplication(sys.argv)
    vars = {}
    def createGui():
        nonlocal vars
        gui = G27CalibGui()
        gui.setMinimumSize(1024, 700)
        gui.setWindowTitle("G27 Pedals and Shifter")
        coll = Collector(vars["tty"])
        coll.valuesChanged.connect(gui.newVals)
        app.aboutToQuit.connect(coll.stop)
        if vars["jsdev"] is not None:
            js = JoystickSink(vars["jsdev"])
            app.aboutToQuit.connect(js.stop)
        else:
            js = None
        gui.sendModeCmd.connect(coll.sendModeCmd)
        gui.show()
        if main_widget is not None: main_widget.hide()
        vars["gui"] = gui
        vars["coll"] = coll
        vars["js"] = js

    if len(list_ports.comports()) != 1 or len(inputs.devices.gamepads) != 1:
        main_widget = QWidget()
        main_widget.setWindowTitle("G27 Pedalsand Shifter - serial port")
        layout = QGridLayout(main_widget)
        layout.addWidget(QLabel("Select Arduino COM port:", main_widget),0,0)
        ttyCombo = QComboBox(main_widget)
        layout.addWidget(ttyCombo,0,1)
        layout.addWidget(QLabel("Select G27 joystick port:", main_widget), 1, 0)
        jsCombo = QComboBox(main_widget)
        layout.addWidget(jsCombo,1,1)
        def refresh():
            ttyCombo.clear()
            n = 0
            for p in list_ports.comports():
                ttyCombo.addItem(p.device)
                n += 1
            k = 0
            jsCombo.clear()
            for i,d in enumerate(inputs.devices.gamepads):
                jsCombo.addItem(d.name, userData=d)
                k += 1
            btnStart.setEnabled(n > 0)
        def startgui():
            idx = ttyCombo.currentIndex()
            vars["tty"] = ttyCombo.itemText(ttyCombo.currentIndex())
            vars["jsdev"] = jsCombo.itemData(jsCombo.currentIndex())
            createGui()
        btnRefresh = QPushButton("Refresh Devices")
        btnRefresh.clicked.connect(refresh)
        layout.addWidget(btnRefresh, 0, 2)
        btnStart = QPushButton("Start")
        btnStart.clicked.connect(startgui)
        refresh()
        layout.addWidget(btnStart, 1, 2)
        main_widget.show()
    else:
        main_widget = None
        vars["tty"] = list_ports.comports()[0].device
        vars["jsdev"] = inputs.devices.gamepads[0]
        createGui()
    os._exit(app.exec_())

if __name__ == "__main__":
    main()
