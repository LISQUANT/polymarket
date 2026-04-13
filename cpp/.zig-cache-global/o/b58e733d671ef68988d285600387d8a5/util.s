.text
.globl logwtmp
.type logwtmp, %function;
.symver logwtmp, logwtmp@@GLIBC_2.2.5
logwtmp:
.globl login
.type login, %function;
.symver login, login@@GLIBC_2.2.5
login:
.globl forkpty
.type forkpty, %function;
.symver forkpty, forkpty@@GLIBC_2.2.5
forkpty:
.globl openpty
.type openpty, %function;
.symver openpty, openpty@@GLIBC_2.2.5
openpty:
.globl login_tty
.type login_tty, %function;
.symver login_tty, login_tty@@GLIBC_2.2.5
login_tty:
.globl logout
.type logout, %function;
.symver logout, logout@@GLIBC_2.2.5
logout:
.data
