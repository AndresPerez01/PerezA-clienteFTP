# Cliente-FTP-Concurrente
PerezA
Cliente FTP en C que soporta transferencias concurrentes usando fork() y pthread para manejar hasta 10 transferencias simultáneas.
Soporta comandos básicos y avanzados del RFC 959: get, put, ls/dir/list, pwd, cd, mkdir, delete, jobs, mode, quit.
Permite modos de transferencia activo (PORT) y pasivo (PASV), manejando conexiones de datos y control correctamente.
Se compila con gcc -o PerezA-clienteFTP PerezA-clienteFTP.c connectsock.c connectTCP.c errexit.c -lpthread y se ejecuta con ./PerezA-clienteFTP <servidor> [puerto].
