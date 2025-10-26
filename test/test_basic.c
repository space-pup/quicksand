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

	uint8_t data_write[5] = {1, 2, 3, 4, 5};
	quicksand_write(writer, data_write, sizeof(data_write));
	int64_t size = 5;
	uint8_t data_read[size];
	int64_t ret = quicksand_read(reader, data_read, &size);
	assert(ret == 0);

	for(int i = 0; i < size; i += 1) {
		assert(data_read[i] == data_write[i]);
	}
	assert(quicksand_read(reader, data_read, &size) == -1);

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
