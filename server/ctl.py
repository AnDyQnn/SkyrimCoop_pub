"""Send a test input command to the local server: python ctl.py press a 200 | stick 0 32767 0 0 1000"""
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(2)
s.sendto(' '.join(sys.argv[1:]).encode(), ('127.0.0.1', 27016))
print(s.recvfrom(4096)[0].decode("utf-8"))
