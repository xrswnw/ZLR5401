"""One-off: verify self-test comm-bit auto-heal (FC=0x0F)."""
import sys

sys.path.insert(0, ".")
from zlr.const import DEV_ADDR, FC_AM_CTRL
from zlr.hid_link import HidLink

FC_SELFTEST_CTRL = 0x0F

AM_QUERY = 0x05
SELFTEST_QUERY = 0x01

ERR_BIT_NAMES = {0: "MOTOR_SPI", 1: "MOTOR_FAULT", 2: "UHF_COMM", 3: "AM_COMM",
                 4: "PARAM_CRC", 5: "TRAVEL_SW"}


def hexs(b):
    return " ".join(f"{x:02X}" for x in b)


def parse_st(data):
    bits = data[2] | (data[3] << 8)
    names = [ERR_BIT_NAMES[i] for i in range(16) if bits & (1 << i)]
    print(f"  errBits=0x{bits:04X} [{'|'.join(names) if names else 'OK'}] "
          f"motorCommOk={data[4]} drvFault=0x{data[5]:02X} "
          f"uhfLink={data[6]} amLink={data[7]} paramCrc={data[8]} switchErr=0x{data[9]:02X}")


def st(link):
    link.send_frame(DEV_ADDR, FC_SELFTEST_CTRL, bytes([SELFTEST_QUERY]))
    try:
        f = link.recv_frame(timeout_s=3.0)
    except Exception as e:
        print(f"  read err: {e}, reopen+retry")
        link.close()
        link.open()
        link.send_frame(DEV_ADDR, FC_SELFTEST_CTRL, bytes([SELFTEST_QUERY]))
        f = link.recv_frame(timeout_s=3.0)
    if f is None:
        print("SELFTEST QUERY: no response")
        return
    print(f"SELFTEST QUERY raw=[{hexs(f['data'])}]")
    parse_st(f["data"])


def am(link):
    link.send_frame(DEV_ADDR, FC_AM_CTRL, bytes([AM_QUERY]))
    try:
        f = link.recv_frame(timeout_s=3.0)
    except Exception as e:
        print(f"  read err: {e}, reopen+retry")
        link.close()
        link.open()
        link.send_frame(DEV_ADDR, FC_AM_CTRL, bytes([AM_QUERY]))
        f = link.recv_frame(timeout_s=3.0)
    print(f"AM QUERY: err={f['data'][1] if f else 'timeout'}")


def main():
    link = HidLink()
    link.open()
    print("== 1. boot state (post-flash, AM probe may have failed) ==")
    st(link)
    print("== 2. AM QUERY (establish link) ==")
    am(link)
    print("== 3. SELFTEST QUERY after AM recovery ==")
    st(link)
    link.close()


if __name__ == "__main__":
    main()
