# Main switch between UNIBUS and QBUS makefile

ifeq ($(QUNILATOR_PLATFORM),UNIBUS)
        include makefile_u
else ifeq  ($(QUNILATOR_PLATFORM),QBUS)
        include makefile_q
else
        $(error Set QUNILATOR_PLATFORM to UNIBUS or QBUS!)
endif
