# spi_memory_systemc

A 4 KiB (constructor-configurable) byte-array memory with a `simple_target_socket`.
`b_transport` reads and writes by address into the array. `get_direct_mem_ptr`
grants read/write DMI over the entire region, giving the initiator a raw `uint8_t *`
for zero-copy access. Its source is compiled by `fss_spi_memory_systemc`; this
directory has no standalone CMake target.
