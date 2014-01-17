include Makefile.inc
SUBDIRS = src tests
.PHONY: subdirs $(SUBDIRS)
subdirs: $(SUBDIRS)
$(SUBDIRS):
	$(MAKE) -C $@

tests: src

clean:
	rm -f deadspy.out.* client.out.* src/*.o src/*.a tests/*.o tests/*.so

check:
	$(PIN_PATH)/pin -t tests/cct_client.so -- ls	
	$(PIN_PATH)/pin -t tests/cct_client_mem_only.so -- ls	
	$(PIN_PATH)/pin -t tests/cct_data_centric_client.so  -- ls	
	$(PIN_PATH)/pin -t tests/cct_data_centric_client_tree_based.so  -- ls	
	$(PIN_PATH)/pin -t tests/deadspy_client.so  -- ls	
	$(PIN_PATH)/pin -t tests/deadspy_client.so  -- tests/deadWrites
