import socket
import random

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(("localhost", 5555))
for i in range(0, 1000000):
    if random.randint(0, 1) == 0:
        s.send("SET larry foo\n")
        s.recv(1024)
    else:
        s.send("GET larry\n")
        s.recv(1024)
s.close()