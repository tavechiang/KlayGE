#include <KlayGE/KlayGE.hpp>
#include <KFL/ThrowErr.hpp>
#include <KFL/Util.hpp>
#include <KFL/Math.hpp>
#include <KlayGE/Font.hpp>
#include <KlayGE/Renderable.hpp>
#include <KlayGE/RenderableHelper.hpp>
#include <KlayGE/RenderEngine.hpp>
#include <KlayGE/RenderEffect.hpp>
#include <KlayGE/FrameBuffer.hpp>
#include <KlayGE/SceneManager.hpp>
#include <KlayGE/Context.hpp>
#include <KlayGE/ResLoader.hpp>
#include <KlayGE/RenderSettings.hpp>
#include <KlayGE/Mesh.hpp>
#include <KlayGE/GraphicsBuffer.hpp>
#include <KlayGE/SceneObjectHelper.hpp>
#include <KlayGE/Query.hpp>
#include <KlayGE/PostProcess.hpp>
#include <KlayGE/Camera.hpp>

#include <KlayGE/RenderFactory.hpp>
#include <KlayGE/InputFactory.hpp>

#include <vector>
#include <sstream>
#include <boost/bind.hpp>

#include "OrderIndependentTransparency.hpp"

using namespace std;
using namespace KlayGE;

#ifdef KLAYGE_COMPILER_MSVC
extern "C"
{
	_declspec(dllexport) uint32_t NvOptimusEnablement = 0x00000001;
}
#endif

namespace
{
	class RenderPolygon : public StaticMesh
	{
	public:
		RenderPolygon(RenderModelPtr model, std::wstring const & name)
			: StaticMesh(model, name)
		{
			RenderFactory& rf = Context::Instance().RenderFactoryInstance();

			no_oit_tech_ = rf.LoadEffect("NoOIT.fxml")->TechniqueByName("NoOIT");
			depth_peeling_1st_tech_ = rf.LoadEffect("DepthPeeling.fxml")->TechniqueByName("DepthPeeling1st");
			depth_peeling_nth_tech_ = depth_peeling_1st_tech_->Effect().TechniqueByName("DepthPeelingNth");
			technique_ = depth_peeling_1st_tech_;
		}

		void BuildMeshInfo()
		{
			AABBox const & pos_bb = this->PosBound();
			*(no_oit_tech_->Effect().ParameterByName("pos_center")) = pos_bb.Center();
			*(no_oit_tech_->Effect().ParameterByName("pos_extent")) = pos_bb.HalfSize();
			*(depth_peeling_1st_tech_->Effect().ParameterByName("pos_center")) = pos_bb.Center();
			*(depth_peeling_1st_tech_->Effect().ParameterByName("pos_extent")) = pos_bb.HalfSize();

			AABBox const & tc_bb = this->TexcoordBound();
			*(no_oit_tech_->Effect().ParameterByName("tc_center")) = float2(tc_bb.Center().x(), tc_bb.Center().y());
			*(no_oit_tech_->Effect().ParameterByName("tc_extent")) = float2(tc_bb.HalfSize().x(), tc_bb.HalfSize().y());
			*(depth_peeling_1st_tech_->Effect().ParameterByName("tc_center")) = float2(tc_bb.Center().x(), tc_bb.Center().y());
			*(depth_peeling_1st_tech_->Effect().ParameterByName("tc_extent")) = float2(tc_bb.HalfSize().x(), tc_bb.HalfSize().y());

			TexturePtr diffuse_tex = SyncLoadTexture("robot-clean_diffuse.dds", EAH_GPU_Read | EAH_Immutable);
			TexturePtr specular_tex = SyncLoadTexture("robot-clean_specular.dds", EAH_GPU_Read | EAH_Immutable);
			TexturePtr normal_tex = SyncLoadTexture("robot-clean_normal.dds", EAH_GPU_Read | EAH_Immutable);
			TexturePtr emit_tex = SyncLoadTexture("robot-clean_selfillumination.dds", EAH_GPU_Read | EAH_Immutable);

			*(no_oit_tech_->Effect().ParameterByName("diffuse_tex")) = diffuse_tex;
			*(no_oit_tech_->Effect().ParameterByName("specular_tex")) = specular_tex;
			*(no_oit_tech_->Effect().ParameterByName("normal_tex")) = normal_tex;
			*(no_oit_tech_->Effect().ParameterByName("emit_tex")) = emit_tex;
			*(depth_peeling_1st_tech_->Effect().ParameterByName("diffuse_tex")) = diffuse_tex;
			*(depth_peeling_1st_tech_->Effect().ParameterByName("specular_tex")) = specular_tex;
			*(depth_peeling_1st_tech_->Effect().ParameterByName("normal_tex")) = normal_tex;
			*(depth_peeling_1st_tech_->Effect().ParameterByName("emit_tex")) = emit_tex;
		}

		void DepthPeelingEnabled(bool dp)
		{
			if (!dp)
			{
				mode_ = OM_No;
				technique_ = no_oit_tech_;
			}
			else
			{
				mode_ = OM_DepthPeeling;
			}
		}

		void FirstPass(bool fp)
		{
			if (fp)
			{
				technique_ = depth_peeling_1st_tech_;
			}
			else
			{
				technique_ = depth_peeling_nth_tech_;
			}
		}

		void LastDepth(TexturePtr const & depth_tex)
		{
			*(technique_->Effect().ParameterByName("last_depth_tex")) = depth_tex;
		}

		void Update()
		{
			App3DFramework const & app = Context::Instance().AppInstance();
			Camera const & camera = app.ActiveCamera();

			float4x4 const & model = float4x4::Identity();

			*(technique_->Effect().ParameterByName("mvp")) = model * camera.ViewProjMatrix();
			*(technique_->Effect().ParameterByName("eye_pos")) = camera.EyePos();

			if (mode_ != OM_No)
			{
				float q = camera.FarPlane() / (camera.FarPlane() - camera.NearPlane());
				*(technique_->Effect().ParameterByName("near_q")) = float2(camera.NearPlane() * q, q);
			}
		}

		void LightPos(float3 const & light_pos)
		{
			*(no_oit_tech_->Effect().ParameterByName("light_pos")) = light_pos;
			*(depth_peeling_1st_tech_->Effect().ParameterByName("light_pos")) = light_pos;
		}

	private:
		OITMode mode_;
		RenderTechniquePtr no_oit_tech_;
		RenderTechniquePtr depth_peeling_1st_tech_;
		RenderTechniquePtr depth_peeling_nth_tech_;
	};

	class PolygonObject : public SceneObjectHelper
	{
	public:
		PolygonObject()
			: SceneObjectHelper(SOA_Cullable)
		{
			renderable_ = SyncLoadModel("robot_clean.meshml", EAH_GPU_Read | EAH_Immutable, CreateModelFactory<RenderModel>(), CreateMeshFactory<RenderPolygon>());
		}

		void LightPos(float3 const & light_pos)
		{
			RenderModelPtr model = checked_pointer_cast<RenderModel>(renderable_);
			for (uint32_t i = 0; i < model->NumMeshes(); ++ i)
			{
				checked_pointer_cast<RenderPolygon>(model->Mesh(i))->LightPos(light_pos);
			}
		}

		void DepthPeelingEnabled(bool dp)
		{
			RenderModelPtr model = checked_pointer_cast<RenderModel>(renderable_);
			for (uint32_t i = 0; i < model->NumMeshes(); ++ i)
			{
				checked_pointer_cast<RenderPolygon>(model->Mesh(i))->DepthPeelingEnabled(dp);
			}
		}

		void FirstPass(bool fp)
		{
			RenderModelPtr model = checked_pointer_cast<RenderModel>(renderable_);
			for (uint32_t i = 0; i < model->NumMeshes(); ++ i)
			{
				checked_pointer_cast<RenderPolygon>(model->Mesh(i))->FirstPass(fp);
			}
		}

		void LastDepth(TexturePtr const & depth_tex)
		{
			RenderModelPtr model = checked_pointer_cast<RenderModel>(renderable_);
			for (uint32_t i = 0; i < model->NumMeshes(); ++ i)
			{
				checked_pointer_cast<RenderPolygon>(model->Mesh(i))->LastDepth(depth_tex);
			}
		}

		void Update(float app_time, float elapsed_time)
		{
			SceneObjectHelper::Update(app_time, elapsed_time);

			RenderModelPtr model = checked_pointer_cast<RenderModel>(renderable_);
			for (uint32_t i = 0; i < model->NumMeshes(); ++ i)
			{
				checked_pointer_cast<RenderPolygon>(model->Mesh(i))->Update();
			}
		}
	};


	enum
	{
		Exit,
	};

	InputActionDefine actions[] =
	{
		InputActionDefine(Exit, KS_Escape),
	};
}


int main()
{
	ResLoader::Instance().AddPath("../../Samples/media/Common");

	Context::Instance().LoadCfg("KlayGE.cfg");

	OrderIndependentTransparencyApp app;
	app.Create();
	app.Run();

	return 0;
}

OrderIndependentTransparencyApp::OrderIndependentTransparencyApp()
			: App3DFramework("Order Independent Transparency"),
				num_layers_(0)
{
	ResLoader::Instance().AddPath("../../Samples/media/OrderIndependentTransparency");
}

bool OrderIndependentTransparencyApp::ConfirmDevice() const
{
	RenderDeviceCaps const & caps = Context::Instance().RenderFactoryInstance().RenderEngineInstance().DeviceCaps();
	if (caps.max_shader_model < 2)
	{
		return false;
	}

	return true;
}

void OrderIndependentTransparencyApp::InitObjects()
{
	// ��������
	font_ = Context::Instance().RenderFactoryInstance().MakeFont("gkai00mp.kfont");

	polygon_ = MakeSharedPtr<PolygonObject>();
	checked_pointer_cast<PolygonObject>(polygon_)->LightPos(float3(-1, 2, 1));
	polygon_->AddToSceneManager();

	this->LookAt(float3(-2.0f, 2.0f, 2.0f), float3(0, 1, 0));
	this->Proj(0.1f, 10);

	RenderFactory& rf = Context::Instance().RenderFactoryInstance();
	RenderEngine& re = rf.RenderEngineInstance();

	peeling_fbs_.resize(9);
	for (size_t i = 0; i < peeling_fbs_.size(); ++ i)
	{
		peeling_fbs_[i] = rf.MakeFrameBuffer();
		peeling_fbs_[i]->GetViewport()->camera = re.CurFrameBuffer()->GetViewport()->camera;
	}
	peeled_texs_.resize(peeling_fbs_.size());
	peeled_views_.resize(peeled_texs_.size());

	for (size_t i = 0; i < oc_queries_.size(); ++ i)
	{
		oc_queries_[i] = checked_pointer_cast<ConditionalRender>(rf.MakeConditionalRender());
	}

	fpcController_.Scalers(0.01f, 0.03f);

	InputEngine& inputEngine(Context::Instance().InputFactoryInstance().InputEngineInstance());
	InputActionMap actionMap;
	actionMap.AddActions(actions, actions + sizeof(actions) / sizeof(actions[0]));

	action_handler_t input_handler = MakeSharedPtr<input_signal>();
	input_handler->connect(boost::bind(&OrderIndependentTransparencyApp::InputHandler, this, _1, _2));
	inputEngine.ActionMap(actionMap, input_handler, true);

	blend_pp_ = LoadPostProcess(ResLoader::Instance().Open("Blend.ppml"), "blend");

	UIManager::Instance().Load(ResLoader::Instance().Open("OrderIndependentTransparency.uiml"));
	dialog_oit_ = UIManager::Instance().GetDialogs()[0];
	dialog_layer_ = UIManager::Instance().GetDialogs()[1];

	id_oit_mode_ = dialog_oit_->IDFromName("OITMode");
	id_ctrl_camera_ = dialog_oit_->IDFromName("CtrlCamera");
	id_layer_combo_ = dialog_layer_->IDFromName("LayerCombo");
	id_layer_tex_ = dialog_layer_->IDFromName("LayerTexButton");

	dialog_oit_->Control<UIComboBox>(id_oit_mode_)->OnSelectionChangedEvent().connect(boost::bind(&OrderIndependentTransparencyApp::OITModeHandler, this, _1));
	this->OITModeHandler(*dialog_oit_->Control<UIComboBox>(id_oit_mode_));
	dialog_oit_->Control<UICheckBox>(id_ctrl_camera_)->OnChangedEvent().connect(boost::bind(&OrderIndependentTransparencyApp::CtrlCameraHandler, this, _1));
	this->CtrlCameraHandler(*dialog_oit_->Control<UICheckBox>(id_ctrl_camera_));

	dialog_layer_->Control<UIComboBox>(id_layer_combo_)->OnSelectionChangedEvent().connect(boost::bind(&OrderIndependentTransparencyApp::LayerChangedHandler, this, _1));
	this->LayerChangedHandler(*dialog_layer_->Control<UIComboBox>(id_layer_combo_));

	for (uint32_t i = 0; i < peeled_texs_.size(); ++ i)
	{
		std::wostringstream stream;
		stream << i << " Layer";
		dialog_layer_->Control<UIComboBox>(id_layer_combo_)->AddItem(stream.str());
	}
}

void OrderIndependentTransparencyApp::OnResize(uint32_t width, uint32_t height)
{
	App3DFramework::OnResize(width, height);

	RenderFactory& rf = Context::Instance().RenderFactoryInstance();

	ElementFormat depth_format;
	if (rf.RenderEngineInstance().DeviceCaps().rendertarget_format_support(EF_D24S8, 1, 0))
	{
		depth_format = EF_D24S8;
	}
	else
	{
		depth_format = EF_D16;
	}
	for (size_t i = 0; i < depth_texs_.size(); ++ i)
	{
		depth_texs_[i] = rf.MakeTexture2D(width, height, 1, 1, depth_format, 1, 0, EAH_GPU_Read | EAH_GPU_Write, nullptr);
		depth_view_[i] = rf.Make2DDepthStencilRenderView(*depth_texs_[i], 0, 1, 0);
	}

	ElementFormat peel_format;
	if (rf.RenderEngineInstance().DeviceCaps().rendertarget_format_support(EF_ABGR16F, 1, 0))
	{
		peel_format = EF_ABGR16F;
	}
	else if (rf.RenderEngineInstance().DeviceCaps().rendertarget_format_support(EF_ABGR8, 1, 0))
	{
		peel_format = EF_ABGR8;
	}
	else
	{
		peel_format = EF_ARGB8;
	}
	for (size_t i = 0; i < peeling_fbs_.size(); ++ i)
	{
		peeled_texs_[i] = rf.MakeTexture2D(width, height, 1, 1, peel_format, 1, 0, EAH_GPU_Read | EAH_GPU_Write, nullptr);
		peeled_views_[i] = rf.Make2DRenderView(*peeled_texs_[i], 0, 1, 0);

		peeling_fbs_[i]->Attach(FrameBuffer::ATT_Color0, peeled_views_[i]);
		peeling_fbs_[i]->Attach(FrameBuffer::ATT_DepthStencil, depth_view_[i % 2]);
	}

	UIManager::Instance().SettleCtrls(width, height);
}

void OrderIndependentTransparencyApp::InputHandler(InputEngine const & /*sender*/, InputAction const & action)
{
	switch (action.first)
	{
	case Exit:
		this->Quit();
		break;
	}
}

void OrderIndependentTransparencyApp::OITModeHandler(KlayGE::UIComboBox const & sender)
{
	oit_mode_ = static_cast<OITMode>(sender.GetSelectedIndex());
}

void OrderIndependentTransparencyApp::CtrlCameraHandler(KlayGE::UICheckBox const & sender)
{
	if (sender.GetChecked())
	{
		fpcController_.AttachCamera(this->ActiveCamera());
	}
	else
	{
		fpcController_.DetachCamera();
	}
}

void OrderIndependentTransparencyApp::LayerChangedHandler(KlayGE::UIComboBox const & sender)
{
	if (sender.GetSelectedIndex() >= 0)
	{
		dialog_layer_->Control<UITexButton>(id_layer_tex_)->SetTexture(peeled_texs_[sender.GetSelectedIndex()]);
	}
}

void OrderIndependentTransparencyApp::DoUpdateOverlay()
{
	SceneManager& sceneMgr(Context::Instance().SceneManagerInstance());

	UIManager::Instance().Render();

	std::wostringstream stream;
	stream.precision(2);
	stream << std::fixed << this->FPS() << " FPS";

	font_->RenderText(0, 0, Color(1, 1, 0, 1), L"Order Independent Transparency", 16);
	font_->RenderText(0, 18, Color(1, 1, 0, 1), stream.str(), 16);

	stream.str(L"");
	stream << sceneMgr.NumRenderablesRendered() << " Renderables "
		<< sceneMgr.NumPrimitivesRendered() << " Primitives "
		<< sceneMgr.NumVerticesRendered() << " Vertices";
	font_->RenderText(0, 36, Color(1, 1, 1, 1), stream.str(), 16);

	stream.str(L"");
	stream << num_layers_ << " Layers";
	font_->RenderText(0, 54, Color(1, 1, 0, 1), stream.str(), 16);
}

uint32_t OrderIndependentTransparencyApp::DoUpdate(uint32_t pass)
{
	RenderEngine& re(Context::Instance().RenderFactoryInstance().RenderEngineInstance());

	if (OM_DepthPeeling == oit_mode_)
	{
		switch (pass)
		{
		case 0:
			num_layers_ = 1;

			checked_pointer_cast<PolygonObject>(polygon_)->FirstPass(true);
			re.BindFrameBuffer(peeling_fbs_[0]);
			peeling_fbs_[0]->Clear(FrameBuffer::CBM_Color | FrameBuffer::CBM_Depth, Color(0, 0, 0, 0), 1, 0);
			return App3DFramework::URV_Need_Flush;

		default:
			if (1 == pass)
			{
				checked_pointer_cast<PolygonObject>(polygon_)->FirstPass(false);
			}

			{
				bool finished = false;

				size_t layer_batch = (pass - 1) / oc_queries_.size() * oc_queries_.size() + 1;
				size_t oc_index = (pass - 1) % oc_queries_.size();
				size_t layer = layer_batch + oc_index;
				if (oc_index > 0)
				{
					if (oc_queries_[oc_index - 1])
					{
						oc_queries_[oc_index - 1]->End();
					}
				}
				if ((oc_index == 0) && (layer_batch > 1))
				{
					if (oc_queries_.back())
					{
						oc_queries_.back()->End();
					}
					for (size_t j = 0; j < oc_queries_.size(); ++ j)
					{
						if (oc_queries_[j] && !oc_queries_[j]->AnySamplesPassed())
						{
							finished = true;
						}
						else
						{
							++ num_layers_;
						}
					}
				}
				if (layer_batch < peeled_texs_.size())
				{
					if (!finished)
					{
						checked_pointer_cast<PolygonObject>(polygon_)->LastDepth(depth_texs_[(layer - 1) % 2]);

						re.BindFrameBuffer(peeling_fbs_[layer]);
						peeling_fbs_[layer]->Clear(FrameBuffer::CBM_Color | FrameBuffer::CBM_Depth, Color(0, 0, 0, 0), 1, 0);

						if (oc_queries_[oc_index])
						{
							oc_queries_[oc_index]->Begin();
						}
					}
				}
				else
				{
					finished = true;
				}

				if (finished)
				{
					re.BindFrameBuffer(FrameBufferPtr());
					Color clear_clr(0.2f, 0.4f, 0.6f, 1);
					if (Context::Instance().Config().graphics_cfg.gamma)
					{
						clear_clr.r() = 0.029f;
						clear_clr.g() = 0.133f;
						clear_clr.b() = 0.325f;
					}
					re.CurFrameBuffer()->Clear(FrameBuffer::CBM_Color | FrameBuffer::CBM_Depth, clear_clr, 1, 0);
					for (size_t i = 0; i < num_layers_; ++ i)
					{
						blend_pp_->InputPin(0, peeled_texs_[num_layers_ - 1 - i]);
						blend_pp_->Apply();
					}

					return App3DFramework::URV_Finished;
				}
				else
				{
					return App3DFramework::URV_Need_Flush;
				}
			}
		}
	}
	else
	{
		checked_pointer_cast<PolygonObject>(polygon_)->DepthPeelingEnabled(false);

		re.BindFrameBuffer(FrameBufferPtr());
		Color clear_clr(0.2f, 0.4f, 0.6f, 1);
		if (Context::Instance().Config().graphics_cfg.gamma)
		{
			clear_clr.r() = 0.029f;
			clear_clr.g() = 0.133f;
			clear_clr.b() = 0.325f;
		}
		re.CurFrameBuffer()->Clear(FrameBuffer::CBM_Color | FrameBuffer::CBM_Depth, clear_clr, 1, 0);
		return App3DFramework::URV_Need_Flush | App3DFramework::URV_Finished;
	}
}