#include <assert.h>
#include <stdio.h>

#include "quicksand.h"

int main()
{
	quicksand_connection *writer = NULL;
	quicksand_connection *reader = NULL;
	quicksand_delete("test", -1);
	int64_t success1 = quicksand_connect(&writer, "test", -1, 32, 100, NULL);
	int64_t success2 = quicksand_connect(&reader, "test", -1, -1, -1, NULL);

	assert(success1 == 0 && writer);
	assert(success2 == 0 && reader);

	printf("writer->buffer->index0 %ld\n", writer->buffer->index);
	printf("reader->buffer->index0 %ld\n", reader->buffer->index);
	uint8_t data_write1[5] = {1, 2, 3, 4, 5};
	quicksand_write(writer, data_write1, sizeof(data_write1));
	printf("writer->buffer->index1 %ld\n", writer->buffer->index);
	printf("reader->buffer->index1 %ld\n", reader->buffer->index);
	uint8_t data_write2[5] = {6, 7, 8, 9, 10};
	quicksand_write(writer, data_write2, sizeof(data_write2));
	printf("writer->buffer->index2 %ld\n", writer->buffer->index);
	printf("reader->buffer->index2 %ld\n", reader->buffer->index);
	int64_t size = 5;
	uint8_t data_read1[size], data_read2[size];
	int64_t read_ret1 = quicksand_read(reader, data_read1, &size);
	assert(read_ret1 >= 0);
	int64_t read_ret2 = quicksand_read(reader, data_read2, &size);
	assert(read_ret2 == 0);

	for(int i = 0; i < size; i += 1) {
		assert(data_read1[i] == data_write1[i]);
		assert(data_read2[i] == data_write2[i]);
	}
	assert(quicksand_read(reader, data_read1, &size) == -1);

	// try to connect with wrong size and make sure it fails.
	quicksand_connection *writer_big = NULL;
	int64_t success3 = quicksand_connect(&writer_big, "test", -1, 32, 257, NULL);
	assert(success3 < 0 && !writer_big);

	// delete the topic (unlink shm) and make sure it succeeds
	quicksand_delete("test", -1);
	int64_t success4 = quicksand_connect(&writer_big, "test", -1, 32, 257, NULL);
	assert(success4 == 0 && writer_big);

	quicksand_disconnect(&reader, NULL);
	quicksand_disconnect(&writer, NULL);
	quicksand_disconnect(&writer_big, NULL);
	quicksand_delete("test", -1);
	return 0;
}
