To compile, run make.

It has RUDP library and two applications which use RUDP. The sample applications are vs_send and vs_recv, a file sending application and a file receiving application.

To run ,

the receiver using ./vs_recv [-d] port

the sender using ./vs_send [-d] host1:port1 [host2:port2] ... file1 [file2]...

When executing both the client and server locally, they should be executed in different directories.



