dtbo-y += b2q_kor_singlex_w00_r12.dtbo # ADDED BY LEGO AUTOMATICALLY: DO NOT SUBMIT
b2q_kor_singlex_w00_r12.dtbo-base := ../../../vendor/qcom/lahaina.dtb ../../../vendor/qcom/lahaina-v2.dtb ../../../vendor/qcom/lahaina-v2.1.dtb # ADDED BY LEGO AUTOMATICALLY: DO NOT SUBMIT
dtbo-y += b2q_kor_singlex_w00_r08.dtbo # ADDED BY LEGO AUTOMATICALLY: DO NOT SUBMIT
b2q_kor_singlex_w00_r08.dtbo-base := ../../../vendor/qcom/lahaina.dtb ../../../vendor/qcom/lahaina-v2.dtb ../../../vendor/qcom/lahaina-v2.1.dtb # ADDED BY LEGO AUTOMATICALLY: DO NOT SUBMIT
dtbo-y += b2q_kor_singlex_w00_r07.dtbo # ADDED BY LEGO AUTOMATICALLY: DO NOT SUBMIT
b2q_kor_singlex_w00_r07.dtbo-base := ../../../vendor/qcom/lahaina.dtb ../../../vendor/qcom/lahaina-v2.dtb ../../../vendor/qcom/lahaina-v2.1.dtb # ADDED BY LEGO AUTOMATICALLY: DO NOT SUBMIT
dtbo-y += b2q_kor_singlex_w00_r05.dtbo # ADDED BY LEGO AUTOMATICALLY: DO NOT SUBMIT
b2q_kor_singlex_w00_r05.dtbo-base := ../../../vendor/qcom/lahaina.dtb ../../../vendor/qcom/lahaina-v2.dtb ../../../vendor/qcom/lahaina-v2.1.dtb # ADDED BY LEGO AUTOMATICALLY: DO NOT SUBMIT
dtbo-y += b2q_kor_singlex_w00_r03.dtbo # ADDED BY LEGO AUTOMATICALLY: DO NOT SUBMIT
b2q_kor_singlex_w00_r03.dtbo-base := ../../../vendor/qcom/lahaina.dtb ../../../vendor/qcom/lahaina-v2.dtb ../../../vendor/qcom/lahaina-v2.1.dtb # ADDED BY LEGO AUTOMATICALLY: DO NOT SUBMIT
dtbo-y += b2q_kor_singlex_w00_r02.dtbo # ADDED BY LEGO AUTOMATICALLY: DO NOT SUBMIT
b2q_kor_singlex_w00_r02.dtbo-base := ../../../vendor/qcom/lahaina.dtb ../../../vendor/qcom/lahaina-v2.dtb ../../../vendor/qcom/lahaina-v2.1.dtb # ADDED BY LEGO AUTOMATICALLY: DO NOT SUBMIT
dtbo-y += b2q_kor_singlex_w00_r01.dtbo # ADDED BY LEGO AUTOMATICALLY: DO NOT SUBMIT
b2q_kor_singlex_w00_r01.dtbo-base := ../../../vendor/qcom/lahaina.dtb ../../../vendor/qcom/lahaina-v2.dtb ../../../vendor/qcom/lahaina-v2.1.dtb # ADDED BY LEGO AUTOMATICALLY: DO NOT SUBMIT
# SPDX-License-Identifier: GPL-2.0

define __sec_dtbo_build
dtbo-$(2) += $(1)
$(1)-base := $(3)
endef

define sec_dtbo_build
$(foreach dtbo, $(1), $(eval $(call __sec_dtbo_build, $(dtbo),$(2),$(3))))
endef

# LAHAINA BASE DTB
QCOM_DTB := ../../../vendor/qcom
SEC_LAHAINA_BASE_DTB := $(QCOM_DTB)/lahaina.dtb $(QCOM_DTB)/lahaina-v2.dtb $(QCOM_DTB)/lahaina-v2.1.dtb

ifeq ($(CONFIG_BUILD_ARM64_DT_OVERLAY),y)
# TODO: Add system dtbo here
endif

clean-files := *.dtb *.reverse.dts *.dtbo

