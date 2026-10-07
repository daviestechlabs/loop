CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -DNDEBUG
.PHONY: all test clean
all: c-campaign-test c-campaign
c-campaign-test: test_campaign.c dnd_campaign.c dnd_campaign.h
	$(CC) $(CFLAGS) test_campaign.c dnd_campaign.c -o $@
c-campaign: c_campaign_main.c dnd_campaign.c dnd_campaign.h
	$(CC) $(CFLAGS) c_campaign_main.c dnd_campaign.c -o $@
test: c-campaign-test c-campaign
	./c-campaign-test
	printf '%s\n' \
	  'id_ok campaign-1' \
	  'scope_ok campaign_canon' \
	  'pron_ok El-MIN-ster' \
	  'meta_ok Curse_of_Strahd 5e 10 5 0' \
	  'char_ok char-1 Ireena npc _ 3 15 40 12 voice-1 clear El-MIN-ster 10,10,10,10,10,10 campaign_canon 0 0' \
	  'scene_npc_ok char-1 Ireena npc _ 3 15 40 12 voice-1 clear _ 10,10,10,10,10,10 campaign_canon 0 0' \
	  'recaps_ok 2' \
	  'session-1 turn-1 12 1000' \
	  'session-2 turn-2 8 2000' \
	  'director_ok 0 _ _ 0 ireena,ismark ireena,ismark' \
	  | ./c-campaign | tee /tmp/c-campaign-cli.out
	grep -q 'OK id_ok' /tmp/c-campaign-cli.out
	grep -q 'OK meta_ok' /tmp/c-campaign-cli.out
	grep -q 'OK char_ok' /tmp/c-campaign-cli.out
	grep -q 'OK recaps_ok' /tmp/c-campaign-cli.out
	grep -q 'OK director_ok' /tmp/c-campaign-cli.out
	@echo ALL PASS c-campaign including CLI
clean:
	rm -f c-campaign-test c-campaign
